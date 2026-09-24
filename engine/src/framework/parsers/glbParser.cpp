// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.02.2026.
//
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "glbParser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <queue>
#include <set>
#include <vector>

#include <draco/compression/decode.h>
#include <draco/core/decoder_buffer.h>
#include <draco/mesh/mesh.h>

#include "core/shape/boundingBox.h"
#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "core/math/vector3.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexFormat.h"
#include "framework/parsers/texture/ktx2Transcoder.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"
#include "stb_image.h"
#include "framework/assets/stbImageFlip.h"

namespace visutwin::canvas
{
    namespace
    {
        struct PackedVertex
        {
            float px, py, pz;
            float nx, ny, nz;
            float u, v;
            float tx, ty, tz, tw;
            float u1, v1;
        };

        struct PackedPointVertex
        {
            float px, py, pz;       // position
            float cr, cg, cb, ca;   // vertex color (RGBA)
        };

        void generateTangents(std::vector<PackedVertex>& vertices, const std::vector<uint32_t>* indices)
        {
            const size_t vertexCount = vertices.size();
            if (vertexCount == 0) {
                return;
            }

            std::vector<Vector3> tan1(vertexCount, Vector3(0.0f, 0.0f, 0.0f));
            std::vector<Vector3> tan2(vertexCount, Vector3(0.0f, 0.0f, 0.0f));

            auto accumulateTriangle = [&](const uint32_t i0, const uint32_t i1, const uint32_t i2) {
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
                    return;
                }

                const auto& v0 = vertices[i0];
                const auto& v1 = vertices[i1];
                const auto& v2 = vertices[i2];

                const Vector3 p0(v0.px, v0.py, v0.pz);
                const Vector3 p1(v1.px, v1.py, v1.pz);
                const Vector3 p2(v2.px, v2.py, v2.pz);

                const float du1 = v1.u - v0.u;
                const float dv1 = v1.v - v0.v;
                const float du2 = v2.u - v0.u;
                const float dv2 = v2.v - v0.v;

                const float det = du1 * dv2 - dv1 * du2;
                if (std::abs(det) <= 1e-8f) {
                    return;
                }

                const float invDet = 1.0f / det;
                const Vector3 e1 = p1 - p0;
                const Vector3 e2 = p2 - p0;

                const Vector3 sdir = (e1 * dv2 - e2 * dv1) * invDet;
                const Vector3 tdir = (e2 * du1 - e1 * du2) * invDet;

                tan1[i0] += sdir;
                tan1[i1] += sdir;
                tan1[i2] += sdir;

                tan2[i0] += tdir;
                tan2[i1] += tdir;
                tan2[i2] += tdir;
            };

            if (indices && !indices->empty()) {
                for (size_t i = 0; i + 2 < indices->size(); i += 3) {
                    accumulateTriangle((*indices)[i], (*indices)[i + 1], (*indices)[i + 2]);
                }
            } else {
                for (uint32_t i = 0; i + 2 < vertexCount; i += 3) {
                    accumulateTriangle(i, i + 1, i + 2);
                }
            }

            for (size_t i = 0; i < vertexCount; ++i) {
                const Vector3 n(vertices[i].nx, vertices[i].ny, vertices[i].nz);
                Vector3 t = tan1[i] - n * n.dot(tan1[i]);
                if (t.lengthSquared() <= 1e-8f) {
                    // Fallback axis in case UVs are degenerate on this vertex.
                    t = std::abs(n.getY()) < 0.999f ? n.cross(Vector3(0.0f, 1.0f, 0.0f)) : n.cross(Vector3(1.0f, 0.0f, 0.0f));
                }
                t = t.normalized();

                const float handedness = (n.cross(t).dot(tan2[i]) < 0.0f) ? -1.0f : 1.0f;

                vertices[i].tx = t.getX();
                vertices[i].ty = t.getY();
                vertices[i].tz = t.getZ();
                vertices[i].tw = handedness;
            }
        }

    } // close anonymous namespace

    // ── Public image-loader callback ─────────────────────────────────────

    bool GlbParser::loadImageData(tinygltf::Image* image,
        const int imageIndex,
        std::string* err,
        std::string* warn,
        const int reqWidth,
        const int reqHeight,
        const unsigned char* bytes,
        const int size,
        void* userData)
    {
        (void)imageIndex;
        (void)warn;
        (void)reqWidth;
        (void)reqHeight;
        (void)userData;

        if (!image || !bytes || size <= 0) {
            if (err) {
                *err = "Invalid image payload";
            }
            return false;
        }

        // KTX2 (KHR_texture_basisu): keep the raw supercompressed bytes verbatim —
        // they are transcoded to a GPU block-compressed format at texture creation.
        if (Ktx2Transcoder::isKtx2(bytes, static_cast<size_t>(size))) {
            image->width = 0;
            image->height = 0;
            image->component = 0;
            image->bits = 8;
            image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
            image->image.assign(bytes, bytes + size);
            return true;
        }

        int width = 0;
        int height = 0;
        int components = 0;
        // Per-thread flip state, restored after the decode so it cannot leak into the
        // next image loaded on this thread (see stbImageFlip.h).
        stbi_uc* decoded = nullptr;
        {
            const StbVerticalFlipScope flipScope(true);
            decoded = stbi_load_from_memory(bytes, size, &width, &height, &components, 0);
        }
        if (!decoded) {
            // Unsupported image format (e.g. Basis .basis payloads).
            // Generate a 1x1 magenta placeholder so the model geometry still loads.
            const char* reason = stbi_failure_reason();
            spdlog::warn("GLB image #{}: stb_image cannot decode ({}), mimeType={} — using placeholder",
                imageIndex, reason ? reason : "unknown", image->mimeType);

            image->width = 1;
            image->height = 1;
            image->component = 4;
            image->bits = 8;
            image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
            image->image = {255, 0, 255, 255}; // magenta RGBA
            return true;
        }

        image->width = width;
        image->height = height;
        image->component = components;
        image->bits = 8;
        image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;

        const size_t decodedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(components);
        image->image.assign(decoded, decoded + decodedSize);
        stbi_image_free(decoded);
        return true;
    }

    namespace { // reopen anonymous namespace

        const tinygltf::Accessor* getAccessor(const tinygltf::Model& model, const int accessorIndex)
        {
            if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) {
                return nullptr;
            }
            return &model.accessors[accessorIndex];
        }

        const tinygltf::BufferView* getBufferView(const tinygltf::Model& model, const tinygltf::Accessor& accessor)
        {
            if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size())) {
                return nullptr;
            }
            return &model.bufferViews[accessor.bufferView];
        }

        int componentBytes(const int componentType)
        {
            switch (componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                return 1;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                return 2;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            case TINYGLTF_COMPONENT_TYPE_INT:
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return 4;
            default:
                return 0;
            }
        }

        int accessorStride(const tinygltf::Model& model, const tinygltf::Accessor& accessor)
        {
            const auto* view = getBufferView(model, accessor);
            const auto inferred = tinygltf::GetNumComponentsInType(accessor.type) * componentBytes(accessor.componentType);
            if (!view || view->byteStride == 0) {
                return inferred;
            }
            return view->byteStride;
        }

        // Returns the accessor's data pointer only if the full extent the readers
        // will touch — offset + (count-1)*stride + elementSize — fits inside the
        // buffer. A malformed/hostile GLB with an inflated count or byteStride
        // must fail here rather than read out of bounds.
        // A glTF node's identity when it has no name: upstream's `node_<index>`
        // (glb-parser createNode). The SAME string has to come out of every place
        // that names a node — the entity the container instantiates, the animation
        // target, the skin's bone list — or an unnamed node exists under one name
        // and is animated under another. Until 2026-09-19 the node payload kept the
        // empty name and parseAnimations SKIPPED any channel whose target was
        // unnamed, so an unnamed animated node (common in exporter output, where
        // only meshes and bones are named) simply did not move.
        std::string glbNodeName(const tinygltf::Model& model, const int nodeIndex)
        {
            const auto& name = model.nodes[static_cast<size_t>(nodeIndex)].name;
            return name.empty() ? "node_" + std::to_string(nodeIndex) : name;
        }

        // Parent of every node, from the children lists (-1 for a root). glTF
        // stores the hierarchy top-down only.
        std::vector<int> glbNodeParents(const tinygltf::Model& model)
        {
            std::vector<int> parents(model.nodes.size(), -1);
            for (size_t i = 0; i < model.nodes.size(); ++i) {
                for (const int child : model.nodes[i].children) {
                    if (child >= 0 && child < static_cast<int>(parents.size())) {
                        parents[static_cast<size_t>(child)] = static_cast<int>(i);
                    }
                }
            }
            return parents;
        }

        // The animation target as a PATH of node names from the node's glTF root
        // down to it, joined with '/', upstream's constructNodePath. A bare name
        // cannot tell two nodes apart that share it in different branches — a
        // left and a right "Wheel", or a skeleton exported twice — and every such
        // scene animated only whichever findByName met first. DefaultAnimBinder
        // walks the path and falls back to the leaf name for tracks that were not
        // produced by this parser.
        std::string glbNodePath(const tinygltf::Model& model, const std::vector<int>& parents, int nodeIndex)
        {
            std::string path = glbNodeName(model, nodeIndex);
            for (int parent = parents[static_cast<size_t>(nodeIndex)]; parent >= 0;
                 parent = parents[static_cast<size_t>(parent)]) {
                path = glbNodeName(model, parent) + "/" + path;
            }
            return path;
        }

        const uint8_t* getAccessorBase(const tinygltf::Model& model, const tinygltf::Accessor& accessor)
        {
            const auto* view = getBufferView(model, accessor);
            if (!view) {
                return nullptr;
            }
            if (view->buffer < 0 || view->buffer >= static_cast<int>(model.buffers.size())) {
                return nullptr;
            }
            const auto& buffer = model.buffers[view->buffer];
            const size_t bufferSize = buffer.data.size();
            const auto offset = static_cast<size_t>(view->byteOffset + accessor.byteOffset);
            if (view->byteOffset > bufferSize || accessor.byteOffset > bufferSize || offset > bufferSize) {
                return nullptr;
            }

            const int numComponents = tinygltf::GetNumComponentsInType(accessor.type);
            const int compBytes = componentBytes(accessor.componentType);
            if (numComponents <= 0 || compBytes <= 0) {
                return nullptr;
            }
            const auto elementSize = static_cast<size_t>(numComponents) * static_cast<size_t>(compBytes);
            const auto stride = static_cast<size_t>(accessorStride(model, accessor));
            const auto count = static_cast<size_t>(accessor.count);
            if (count > 0) {
                // Required extent: offset + (count-1)*stride + elementSize <= bufferSize,
                // rearranged to avoid overflow in the multiplication.
                if (elementSize > bufferSize || offset > bufferSize - elementSize) {
                    return nullptr;
                }
                if (count > 1 && stride > 0 &&
                    (count - 1) > (bufferSize - elementSize - offset) / stride) {
                    return nullptr;
                }
            }
            return buffer.data.data() + offset;
        }

        // One component of one element, de-quantised. glTF stores an attribute as
        // float OR as an integer type, raw or normalized: TEXCOORD_n and COLOR_n
        // may be normalized byte/short in CORE glTF, and KHR_mesh_quantization
        // extends that to POSITION, NORMAL and TANGENT. The de-quantisation is the
        // spec's: an unsigned normalized value divides by the type's maximum, a
        // signed one divides by its largest magnitude and clamps at -1 (the extra
        // negative step is not part of the range), and a value that is not
        // normalized is the integer itself — for a quantised POSITION the node
        // transform carries the scale back.
        float decodeComponent(const uint8_t* element, const int componentType,
            const bool normalized, const int component)
        {
            switch (componentType) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return reinterpret_cast<const float*>(element)[component];
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
                const auto value = static_cast<float>(element[component]);
                return normalized ? value / 255.0f : value;
            }
            case TINYGLTF_COMPONENT_TYPE_BYTE: {
                const auto value = static_cast<float>(reinterpret_cast<const int8_t*>(element)[component]);
                return normalized ? std::max(value / 127.0f, -1.0f) : value;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                const auto value = static_cast<float>(reinterpret_cast<const uint16_t*>(element)[component]);
                return normalized ? value / 65535.0f : value;
            }
            case TINYGLTF_COMPONENT_TYPE_SHORT: {
                const auto value = static_cast<float>(reinterpret_cast<const int16_t*>(element)[component]);
                return normalized ? std::max(value / 32767.0f, -1.0f) : value;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                return static_cast<float>(reinterpret_cast<const uint32_t*>(element)[component]);
            case TINYGLTF_COMPONENT_TYPE_INT:
                return static_cast<float>(reinterpret_cast<const int32_t*>(element)[component]);
            default:
                return 0.0f;
            }
        }

        // Reads `count` components of one element into `out`, whatever the accessor's
        // component type. The accessor's TYPE (VEC2/VEC3/...) still has to be the one
        // the caller asked for: a reader that quietly accepted a VEC2 where a VEC3 was
        // wanted would read a neighbouring element's bytes as the third component.
        bool readElement(const tinygltf::Model& model, const tinygltf::Accessor& accessor,
            const size_t index, const int expectedType, float* out, const int count)
        {
            if (accessor.type != expectedType || index >= static_cast<size_t>(accessor.count)) {
                return false;
            }
            const auto* base = getAccessorBase(model, accessor);
            if (!base) {
                return false;
            }
            const auto stride = accessorStride(model, accessor);
            const auto* element = base + index * static_cast<size_t>(stride);
            for (int c = 0; c < count; ++c) {
                out[c] = decodeComponent(element, accessor.componentType, accessor.normalized, c);
            }
            return true;
        }

        bool readFloatVec3(const tinygltf::Model& model, const tinygltf::Accessor& accessor, const size_t index, Vector3& out)
        {
            float v[3];
            if (!readElement(model, accessor, index, TINYGLTF_TYPE_VEC3, v, 3)) {
                return false;
            }
            out = Vector3(v[0], v[1], v[2]);
            return true;
        }

        bool readFloatVec2(const tinygltf::Model& model, const tinygltf::Accessor& accessor, const size_t index, float& u, float& v)
        {
            float value[2];
            if (!readElement(model, accessor, index, TINYGLTF_TYPE_VEC2, value, 2)) {
                return false;
            }
            u = value[0];
            v = value[1];
            return true;
        }

        bool readFloatVec4(const tinygltf::Model& model, const tinygltf::Accessor& accessor, const size_t index, Vector4& out)
        {
            float v[4];
            if (!readElement(model, accessor, index, TINYGLTF_TYPE_VEC4, v, 4)) {
                return false;
            }
            out = Vector4(v[0], v[1], v[2], v[3]);
            return true;
        }

        // Apply glTF sparse-accessor overrides (indices + values bufferViews) on top
        // of `out`, which already holds the base data (zeros when the accessor has no
        // base bufferView, as is typical for sparse morph-target deltas).
        bool applySparseOverrides(const tinygltf::Model& model, const tinygltf::Accessor& accessor,
            const int numComponents, std::vector<float>& out)
        {
            const auto& sparse = accessor.sparse;
            const auto sparseCount = static_cast<size_t>(sparse.count);
            if (sparseCount == 0) {
                return true;
            }

            const auto viewBytes = [&](const int viewIndex, const size_t byteOffset,
                const size_t byteLength) -> const uint8_t* {
                if (viewIndex < 0 || viewIndex >= static_cast<int>(model.bufferViews.size())) {
                    return nullptr;
                }
                const auto& view = model.bufferViews[static_cast<size_t>(viewIndex)];
                if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size())) {
                    return nullptr;
                }
                const auto& buffer = model.buffers[static_cast<size_t>(view.buffer)];
                const size_t bufferSize = buffer.data.size();
                if (view.byteOffset > bufferSize || byteOffset > bufferSize - view.byteOffset) {
                    return nullptr;
                }
                const size_t offset = view.byteOffset + byteOffset;
                if (byteLength > bufferSize - offset) {
                    return nullptr;
                }
                return buffer.data.data() + offset;
            };

            const int idxBytes = componentBytes(sparse.indices.componentType);
            if (idxBytes != 1 && idxBytes != 2 && idxBytes != 4) {
                return false;
            }
            // The sparse VALUES carry the accessor's own component type, so they are
            // de-quantised exactly like the dense data they override.
            const int valueBytes = componentBytes(accessor.componentType);
            if (valueBytes <= 0) {
                return false;
            }
            const auto* idxPtr = viewBytes(sparse.indices.bufferView, sparse.indices.byteOffset,
                sparseCount * static_cast<size_t>(idxBytes));
            const auto valueStride = static_cast<size_t>(numComponents) * static_cast<size_t>(valueBytes);
            const auto* valPtr = viewBytes(sparse.values.bufferView, sparse.values.byteOffset,
                sparseCount * valueStride);
            if (!idxPtr || !valPtr) {
                return false;
            }

            const size_t elementCount = out.size() / static_cast<size_t>(numComponents);
            for (size_t i = 0; i < sparseCount; ++i) {
                size_t index = 0;
                switch (idxBytes) {
                    case 1: index = idxPtr[i]; break;
                    case 2: index = reinterpret_cast<const uint16_t*>(idxPtr)[i]; break;
                    default: index = reinterpret_cast<const uint32_t*>(idxPtr)[i]; break;
                }
                if (index >= elementCount) {
                    return false;
                }
                const auto* element = valPtr + i * valueStride;
                for (int c = 0; c < numComponents; ++c) {
                    out[index * static_cast<size_t>(numComponents) + static_cast<size_t>(c)] =
                        decodeComponent(element, accessor.componentType, accessor.normalized, c);
                }
            }
            return true;
        }

        // Read all data from an accessor into a flat vector of floats.
        // Works for SCALAR, VEC2, VEC3, VEC4, and for every component type glTF
        // allows — the values are de-quantised on the way out.
        // Supports sparse accessors, including the base-less form (bufferView absent,
        // base = zeros) that morph-target deltas commonly use.
        // De-quantises like readElement: an animation sampler's output may be
        // normalized byte/short in core glTF, and a morph target's deltas may be
        // quantised under KHR_mesh_quantization.
        bool readFloatArray(const tinygltf::Model& model, const tinygltf::Accessor& accessor, std::vector<float>& out)
        {
            const int numComponents = tinygltf::GetNumComponentsInType(accessor.type);
            if (numComponents <= 0 || componentBytes(accessor.componentType) <= 0) {
                return false;
            }
            const size_t count = static_cast<size_t>(accessor.count);
            out.assign(count * static_cast<size_t>(numComponents), 0.0f);

            if (accessor.bufferView >= 0) {
                const auto* base = getAccessorBase(model, accessor);
                if (!base) {
                    return false;
                }
                const auto stride = accessorStride(model, accessor);
                for (size_t i = 0; i < count; ++i) {
                    const auto* element = base + i * static_cast<size_t>(stride);
                    for (int c = 0; c < numComponents; ++c) {
                        out[i * static_cast<size_t>(numComponents) + static_cast<size_t>(c)] =
                            decodeComponent(element, accessor.componentType, accessor.normalized, c);
                    }
                }
            } else if (!accessor.sparse.isSparse) {
                return false;
            }

            if (accessor.sparse.isSparse) {
                return applySparseOverrides(model, accessor, numComponents, out);
            }
            return true;
        }

        // ── GPU skinning + morph target extraction ──────────────────────

        /// Skinned vertex layout: PackedVertex (14 floats) + blendWeights (4) +
        /// blendIndices (4) = 88 bytes. Matches the STRIDE_SKINNED vertex descriptor
        /// (attributes 11/12) and the VT_FEATURE_SKINNING shader path.
        constexpr size_t SKINNED_VERTEX_STRIDE = sizeof(PackedVertex) + 8 * sizeof(float);

        /// Per-vertex skin influences read from JOINTS_0/WEIGHTS_0 (4 per vertex each).
        struct SkinAttributes
        {
            std::vector<float> weights;
            std::vector<float> joints;  // Joint indices carried as float (u8/u16 fit exactly).
            bool valid = false;
        };

        SkinAttributes readSkinAttributes(const tinygltf::Model& model,
            const tinygltf::Primitive& primitive, const size_t vertexCount)
        {
            SkinAttributes out;
            if (!primitive.attributes.contains("JOINTS_0") ||
                !primitive.attributes.contains("WEIGHTS_0")) {
                return out;
            }
            const auto* jointsAccessor = getAccessor(model, primitive.attributes.at("JOINTS_0"));
            const auto* weightsAccessor = getAccessor(model, primitive.attributes.at("WEIGHTS_0"));
            if (!jointsAccessor || !weightsAccessor ||
                jointsAccessor->type != TINYGLTF_TYPE_VEC4 ||
                weightsAccessor->type != TINYGLTF_TYPE_VEC4 ||
                static_cast<size_t>(jointsAccessor->count) < vertexCount ||
                static_cast<size_t>(weightsAccessor->count) < vertexCount) {
                spdlog::warn("GLB skin attributes present but malformed — skinning skipped");
                return out;
            }
            const auto* jointsBase = getAccessorBase(model, *jointsAccessor);
            const auto* weightsBase = getAccessorBase(model, *weightsAccessor);
            if (!jointsBase || !weightsBase) {
                return out;
            }
            const auto jointsStride = static_cast<size_t>(accessorStride(model, *jointsAccessor));
            const auto weightsStride = static_cast<size_t>(accessorStride(model, *weightsAccessor));

            out.joints.resize(vertexCount * 4);
            out.weights.resize(vertexCount * 4);
            for (size_t i = 0; i < vertexCount; ++i) {
                const auto* jointsPtr = jointsBase + i * jointsStride;
                for (int c = 0; c < 4; ++c) {
                    float joint = 0.0f;
                    switch (jointsAccessor->componentType) {
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                            joint = static_cast<float>(jointsPtr[c]);
                            break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                            joint = static_cast<float>(reinterpret_cast<const uint16_t*>(jointsPtr)[c]);
                            break;
                        default:
                            return out;  // Spec allows only u8/u16 joints.
                    }
                    out.joints[i * 4 + static_cast<size_t>(c)] = joint;
                }

                const auto* weightsPtr = weightsBase + i * weightsStride;
                float w[4] = {0, 0, 0, 0};
                for (int c = 0; c < 4; ++c) {
                    switch (weightsAccessor->componentType) {
                        case TINYGLTF_COMPONENT_TYPE_FLOAT:
                            w[c] = reinterpret_cast<const float*>(weightsPtr)[c];
                            break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                            w[c] = static_cast<float>(weightsPtr[c]) / 255.0f;
                            break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                            w[c] = static_cast<float>(reinterpret_cast<const uint16_t*>(weightsPtr)[c]) / 65535.0f;
                            break;
                        default:
                            return out;
                    }
                }
                // Renormalize (quantized weights rarely sum to exactly 1); a degenerate
                // all-zero vertex binds fully to its first joint instead of collapsing
                // to the origin.
                const float sum = w[0] + w[1] + w[2] + w[3];
                if (sum > 1e-6f) {
                    const float inv = 1.0f / sum;
                    for (float& c : w) c *= inv;
                } else {
                    w[0] = 1.0f;
                }
                for (int c = 0; c < 4; ++c) {
                    out.weights[i * 4 + static_cast<size_t>(c)] = w[c];
                }
            }
            out.valid = true;
            return out;
        }

        /// Interleave base vertices with skin influences into the 88-byte layout.
        std::vector<uint8_t> packSkinnedVertices(const std::vector<PackedVertex>& vertices,
            const SkinAttributes& skin)
        {
            std::vector<uint8_t> bytes(vertices.size() * SKINNED_VERTEX_STRIDE);
            for (size_t i = 0; i < vertices.size(); ++i) {
                auto* dst = bytes.data() + i * SKINNED_VERTEX_STRIDE;
                std::memcpy(dst, &vertices[i], sizeof(PackedVertex));
                std::memcpy(dst + sizeof(PackedVertex), skin.weights.data() + i * 4, 4 * sizeof(float));
                std::memcpy(dst + sizeof(PackedVertex) + 4 * sizeof(float), skin.joints.data() + i * 4, 4 * sizeof(float));
            }
            return bytes;
        }

        /// Read glTF primitive morph targets (POSITION/NORMAL deltas).
        std::vector<MorphTarget> readMorphTargets(const tinygltf::Model& model,
            const tinygltf::Primitive& primitive, const size_t vertexCount)
        {
            std::vector<MorphTarget> targets;
            for (size_t t = 0; t < primitive.targets.size(); ++t) {
                const auto& target = primitive.targets[t];
                MorphTarget morphTarget;
                morphTarget.name = "target_" + std::to_string(t);

                const auto posIt = target.find("POSITION");
                if (posIt != target.end()) {
                    if (const auto* accessor = getAccessor(model, posIt->second)) {
                        readFloatArray(model, *accessor, morphTarget.deltaPositions);
                    }
                }
                if (morphTarget.deltaPositions.size() < vertexCount * 3) {
                    spdlog::warn("GLB morph target {} has incomplete POSITION deltas — skipped", t);
                    continue;
                }
                const auto nrmIt = target.find("NORMAL");
                if (nrmIt != target.end()) {
                    if (const auto* accessor = getAccessor(model, nrmIt->second)) {
                        readFloatArray(model, *accessor, morphTarget.deltaNormals);
                    }
                }
                targets.push_back(std::move(morphTarget));
            }
            return targets;
        }

        /// Parse glTF skins into container payloads (inverse bind matrices + joint indices).
        void parseSkins(const tinygltf::Model& model, GlbContainerResource* container)
        {
            for (const auto& skin : model.skins) {
                GlbSkinPayload payload;
                payload.jointNodeIndices = skin.joints;

                std::vector<Matrix4> inverseBindPose;
                inverseBindPose.reserve(skin.joints.size());
                std::vector<float> ibmData;
                if (skin.inverseBindMatrices >= 0) {
                    if (const auto* accessor = getAccessor(model, skin.inverseBindMatrices)) {
                        readFloatArray(model, *accessor, ibmData);
                    }
                }
                for (size_t j = 0; j < skin.joints.size(); ++j) {
                    if (ibmData.size() >= (j + 1) * 16) {
                        // glTF matrices are column-major — same as Matrix4.
                        const float* m = ibmData.data() + j * 16;
                        inverseBindPose.emplace_back(
                            Vector4(m[0], m[1], m[2], m[3]),
                            Vector4(m[4], m[5], m[6], m[7]),
                            Vector4(m[8], m[9], m[10], m[11]),
                            Vector4(m[12], m[13], m[14], m[15]));
                    } else {
                        inverseBindPose.push_back(Matrix4::identity());
                    }
                }

                std::vector<std::string> boneNames;
                boneNames.reserve(skin.joints.size());
                for (const int jointNodeIndex : skin.joints) {
                    boneNames.push_back(
                        (jointNodeIndex >= 0 && jointNodeIndex < static_cast<int>(model.nodes.size()))
                            ? glbNodeName(model, jointNodeIndex) : std::string());
                }

                payload.skin = std::make_shared<Skin>(std::move(inverseBindPose), std::move(boneNames));

                // Per-bone AABBs over the bind-space vertices each joint influences.
                // Enables frustum culling of skinned meshes: at runtime the world
                // bound is the union of bone AABBs transformed by
                // bone.worldTransform * inverseBind (see MeshInstance::aabb()).
                {
                    const size_t jointCount = skin.joints.size();
                    std::vector<Vector3> mins(jointCount, Vector3(std::numeric_limits<float>::max()));
                    std::vector<Vector3> maxs(jointCount, Vector3(std::numeric_limits<float>::lowest()));
                    std::vector<uint8_t> used(jointCount, 0);
                    const int skinIndex = static_cast<int>(container->skinPayloadCount());

                    for (const auto& node : model.nodes) {
                        if (node.skin != skinIndex || node.mesh < 0 ||
                            node.mesh >= static_cast<int>(model.meshes.size())) {
                            continue;
                        }
                        for (const auto& primitive : model.meshes[static_cast<size_t>(node.mesh)].primitives) {
                            const auto posIt = primitive.attributes.find("POSITION");
                            if (posIt == primitive.attributes.end()) {
                                continue;
                            }
                            const auto* posAccessor = getAccessor(model, posIt->second);
                            if (!posAccessor) {
                                continue;
                            }
                            std::vector<float> positions;
                            if (!readFloatArray(model, *posAccessor, positions)) {
                                continue;
                            }
                            const size_t vertexCount = positions.size() / 3;
                            const auto attributes = readSkinAttributes(model, primitive, vertexCount);
                            if (attributes.joints.size() < vertexCount * 4 ||
                                attributes.weights.size() < vertexCount * 4) {
                                continue;
                            }
                            // A morphed skin: each vertex can also move by its morph deltas, so
                            // the bone boxes take the vertex's reach under every target at once
                            // — the negative deltas summed toward the min, the positive toward
                            // the max, per axis — as upstream's _initBoneAabbs does. Without it
                            // a skinned mesh whose targets push it outward was culled on screen.
                            const auto morphTargets = readMorphTargets(model, primitive, vertexCount);
                            for (size_t v = 0; v < vertexCount; ++v) {
                                const Vector3 rest = Vector3::load(&positions[v * 3]);
                                Vector3 reachMin = rest;
                                Vector3 reachMax = rest;
                                for (const auto& target : morphTargets) {
                                    const Vector3 delta = Vector3::load(&target.deltaPositions[v * 3]);
                                    reachMin += Vector3::min(delta, Vector3(0.0f, 0.0f, 0.0f));
                                    reachMax += Vector3::max(delta, Vector3(0.0f, 0.0f, 0.0f));
                                }
                                for (int k = 0; k < 4; ++k) {
                                    const float weight = attributes.weights[v * 4 + static_cast<size_t>(k)];
                                    if (weight <= 1e-4f) {
                                        continue;
                                    }
                                    const auto joint = static_cast<size_t>(attributes.joints[v * 4 + static_cast<size_t>(k)]);
                                    if (joint >= jointCount) {
                                        continue;
                                    }
                                    used[joint] = 1;
                                    mins[joint] = Vector3::min(mins[joint], reachMin);
                                    maxs[joint] = Vector3::max(maxs[joint], reachMax);
                                }
                            }
                        }
                    }

                    std::vector<BoundingBox> boneAabbs(jointCount);
                    bool anyUsed = false;
                    for (size_t j = 0; j < jointCount; ++j) {
                        if (!used[j]) {
                            continue;
                        }
                        anyUsed = true;
                        boneAabbs[j].setCenter((mins[j] + maxs[j]) * 0.5f);
                        boneAabbs[j].setHalfExtents((maxs[j] - mins[j]) * 0.5f);
                    }
                    if (anyUsed) {
                        payload.skin->setBoneAabbs(std::move(boneAabbs), std::move(used));
                    }
                }

                container->addSkinPayload(payload);
            }
            if (!model.skins.empty()) {
                spdlog::info("  Parsed {} skin(s)", model.skins.size());
            }
        }

        // Parse glTF animations into AnimTrack objects stored on the container.
        //
        // Overload: output animation tracks to a map (thread-safe — no container needed).
        void parseAnimations(const tinygltf::Model& model,
            std::unordered_map<std::string, std::shared_ptr<AnimTrack>>& outTracks)
        {
            if (model.animations.empty()) {
                return;
            }

            const std::vector<int> nodeParents = glbNodeParents(model);

            for (size_t animIdx = 0; animIdx < model.animations.size(); ++animIdx) {
                const auto& anim = model.animations[animIdx];

                std::string trackName = anim.name.empty()
                    ? ("animation_" + std::to_string(animIdx))
                    : anim.name;

                float duration = 0.0f;
                auto track = std::make_shared<AnimTrack>();

                for (const auto& channel : anim.channels) {
                    if (channel.target_node < 0 ||
                        channel.target_node >= static_cast<int>(model.nodes.size())) {
                        continue;
                    }
                    if (channel.sampler < 0 ||
                        channel.sampler >= static_cast<int>(anim.samplers.size())) {
                        continue;
                    }

                    const auto& sampler = anim.samplers[channel.sampler];
                    const auto* inputAccessor = getAccessor(model, sampler.input);
                    const auto* outputAccessor = getAccessor(model, sampler.output);
                    if (!inputAccessor || !outputAccessor) {
                        continue;
                    }

                    // Map glTF target path to upstream property path.
                    std::string propertyPath;
                    int outputComponents = 0;
                    if (channel.target_path == "translation") {
                        propertyPath = "localPosition";
                        outputComponents = 3;
                    } else if (channel.target_path == "rotation") {
                        propertyPath = "localRotation";
                        outputComponents = 4;
                    } else if (channel.target_path == "scale") {
                        propertyPath = "localScale";
                        outputComponents = 3;
                    } else if (channel.target_path == "weights") {
                        // Morph target weights: one output component per target of the
                        // node's mesh (glTF: output count = keyCount * targetCount).
                        propertyPath = "weights";
                        const auto& targetNode = model.nodes[static_cast<size_t>(channel.target_node)];
                        if (targetNode.mesh < 0 ||
                            targetNode.mesh >= static_cast<int>(model.meshes.size())) {
                            continue;
                        }
                        const auto& mesh = model.meshes[static_cast<size_t>(targetNode.mesh)];
                        if (mesh.primitives.empty() || mesh.primitives[0].targets.empty()) {
                            continue;
                        }
                        outputComponents = static_cast<int>(mesh.primitives[0].targets.size());
                    } else {
                        continue;
                    }

                    // Map interpolation mode.
                    AnimInterpolation interpMode = AnimInterpolation::LINEAR;
                    if (sampler.interpolation == "STEP") {
                        interpMode = AnimInterpolation::STEP;
                    } else if (sampler.interpolation == "CUBICSPLINE") {
                        interpMode = AnimInterpolation::CUBIC;
                    }

                    // Read input (keyframe times).
                    AnimData inputData;
                    inputData.components = 1;
                    if (!readFloatArray(model, *inputAccessor, inputData.data)) {
                        continue;
                    }

                    // Track max time for duration.
                    if (!inputData.data.empty()) {
                        duration = std::max(duration, inputData.data.back());
                    }

                    // Read output (values).
                    AnimData outputData;
                    if (interpMode == AnimInterpolation::CUBIC) {
                        // CUBICSPLINE stores 3 values per keyframe: [inTangent, value, outTangent].
                        // The accessor has count == keyframe_count, but each element has
                        // 3 * outputComponents floats.
                        outputData.components = outputComponents;
                        if (!readFloatArray(model, *outputAccessor, outputData.data)) {
                            continue;
                        }
                    } else {
                        outputData.components = outputComponents;
                        if (!readFloatArray(model, *outputAccessor, outputData.data)) {
                            continue;
                        }
                    }

                    // Quaternion winding normalization for rotation channels.
                    // Ensures shortest-path slerp: if dot(q[i], q[i+1]) < 0, negate q[i+1].
                    if (propertyPath == "localRotation" && interpMode != AnimInterpolation::CUBIC) {
                        const size_t quatCount = outputData.count();
                        for (size_t i = 1; i < quatCount; ++i) {
                            const size_t prev = (i - 1) * 4;
                            const size_t curr = i * 4;
                            const float dot = outputData.data[prev] * outputData.data[curr] +
                                              outputData.data[prev + 1] * outputData.data[curr + 1] +
                                              outputData.data[prev + 2] * outputData.data[curr + 2] +
                                              outputData.data[prev + 3] * outputData.data[curr + 3];
                            if (dot < 0.0f) {
                                outputData.data[curr]     = -outputData.data[curr];
                                outputData.data[curr + 1] = -outputData.data[curr + 1];
                                outputData.data[curr + 2] = -outputData.data[curr + 2];
                                outputData.data[curr + 3] = -outputData.data[curr + 3];
                            }
                        }
                    }

                    // Target node as a name path (see glbNodePath); an unnamed node
                    // takes the same fallback name its entity is instantiated under.
                    const std::string nodeName = glbNodePath(model, nodeParents, channel.target_node);

                    // Create curve referencing the input/output by index.
                    const size_t inputIndex = track->inputs().size();
                    const size_t outputIndex = track->outputs().size();

                    track->addInput(std::move(inputData));
                    track->addOutput(std::move(outputData));

                    AnimCurve curve;
                    curve.nodeName = nodeName;
                    curve.propertyPath = propertyPath;
                    curve.inputIndex = inputIndex;
                    curve.outputIndex = outputIndex;
                    curve.interpolation = interpMode;
                    track->addCurve(curve);
                }

                track->setName(trackName);
                track->setDuration(duration);

                if (!track->curves().empty()) {
                    outTracks[trackName] = track;
                    spdlog::info("  Parsed animation '{}': {:.2f}s, {} curves",
                        trackName, duration, track->curves().size());
                }
            }
        }

        // Overload: output animation tracks to a container (existing behavior).
        bool readIndices(const tinygltf::Model& model, const tinygltf::Accessor& accessor, std::vector<uint32_t>& out)
        {
            if (accessor.type != TINYGLTF_TYPE_SCALAR) {
                return false;
            }
            const auto* base = getAccessorBase(model, accessor);
            if (!base) {
                return false;
            }
            const auto stride = accessorStride(model, accessor);
            out.resize(static_cast<size_t>(accessor.count));

            for (size_t i = 0; i < out.size(); ++i) {
                const auto* src = base + i * static_cast<size_t>(stride);
                uint32_t value = 0;
                switch (accessor.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    value = *reinterpret_cast<const uint8_t*>(src);
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    value = *reinterpret_cast<const uint16_t*>(src);
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    value = *reinterpret_cast<const uint32_t*>(src);
                    break;
                default:
                    return false;
                }
                out[i] = value;
            }
            return true;
        }

        bool primitiveUsesDraco(const tinygltf::Primitive& primitive)
        {
            return primitive.extensions.contains("KHR_draco_mesh_compression");
        }

        int readDracoAttributeId(const tinygltf::Value& dracoExtension, const std::string& semantic)
        {
            if (!dracoExtension.IsObject() || !dracoExtension.Has("attributes")) {
                return -1;
            }
            const auto attrs = dracoExtension.Get("attributes");
            if (!attrs.IsObject() || !attrs.Has(semantic)) {
                return -1;
            }
            const auto& idValue = attrs.Get(semantic);
            if (idValue.IsInt()) {
                return idValue.Get<int>();
            }
            if (idValue.IsNumber()) {
                return idValue.GetNumberAsInt();
            }
            return -1;
        }

        const draco::PointAttribute* getDracoAttribute(const draco::Mesh& mesh, const tinygltf::Value& dracoExtension,
            const std::string& semantic)
        {
            const int uniqueId = readDracoAttributeId(dracoExtension, semantic);
            if (uniqueId < 0) {
                return nullptr;
            }
            return mesh.GetAttributeByUniqueId(uniqueId);
        }

        bool decodeDracoPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive,
            std::vector<PackedVertex>& outVertices, std::vector<uint32_t>& outIndices, Vector3& outMinPos, Vector3& outMaxPos)
        {
            const auto extIt = primitive.extensions.find("KHR_draco_mesh_compression");
            if (extIt == primitive.extensions.end()) {
                return false;
            }
            const auto& dracoExt = extIt->second;
            if (!dracoExt.IsObject() || !dracoExt.Has("bufferView")) {
                spdlog::warn("glTF primitive has malformed KHR_draco_mesh_compression extension");
                return false;
            }

            const auto bufferViewVal = dracoExt.Get("bufferView");
            const int bufferViewIndex = bufferViewVal.IsInt() ? bufferViewVal.Get<int>() :
                (bufferViewVal.IsNumber() ? bufferViewVal.GetNumberAsInt() : -1);
            if (bufferViewIndex < 0 || bufferViewIndex >= static_cast<int>(model.bufferViews.size())) {
                spdlog::warn("Draco primitive references invalid bufferView {}", bufferViewIndex);
                return false;
            }

            const auto& view = model.bufferViews[static_cast<size_t>(bufferViewIndex)];
            if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size())) {
                spdlog::warn("Draco primitive bufferView references invalid buffer {}", view.buffer);
                return false;
            }

            const auto& buffer = model.buffers[static_cast<size_t>(view.buffer)];
            const size_t byteOffset = static_cast<size_t>(view.byteOffset);
            const size_t byteLength = static_cast<size_t>(view.byteLength);
            if (byteOffset + byteLength > buffer.data.size()) {
                spdlog::warn("Draco primitive compressed payload out of bounds");
                return false;
            }

            draco::DecoderBuffer decoderBuffer;
            decoderBuffer.Init(reinterpret_cast<const char*>(buffer.data.data() + byteOffset),
                static_cast<int64_t>(byteLength));

            draco::Decoder decoder;
            auto meshStatus = decoder.DecodeMeshFromBuffer(&decoderBuffer);
            if (!meshStatus.ok()) {
                spdlog::warn("Failed to decode Draco mesh: {}", meshStatus.status().error_msg_string());
                return false;
            }

            std::unique_ptr<draco::Mesh> dracoMesh = std::move(meshStatus).value();
            if (!dracoMesh || dracoMesh->num_points() <= 0) {
                spdlog::warn("Decoded Draco mesh has no points");
                return false;
            }

            const auto* positionAttr = getDracoAttribute(*dracoMesh, dracoExt, "POSITION");
            if (!positionAttr || positionAttr->num_components() < 3) {
                spdlog::warn("Decoded Draco mesh missing POSITION attribute");
                return false;
            }
            const auto* normalAttr = getDracoAttribute(*dracoMesh, dracoExt, "NORMAL");
            const auto* uvAttr = getDracoAttribute(*dracoMesh, dracoExt, "TEXCOORD_0");
            const auto* uv1Attr = getDracoAttribute(*dracoMesh, dracoExt, "TEXCOORD_1");
            const auto* tangentAttr = getDracoAttribute(*dracoMesh, dracoExt, "TANGENT");

            const int32_t pointCount = dracoMesh->num_points();
            outVertices.resize(static_cast<size_t>(pointCount));

            outMinPos = Vector3(std::numeric_limits<float>::max());
            outMaxPos = Vector3(std::numeric_limits<float>::lowest());

            for (int32_t i = 0; i < pointCount; ++i) {
                const draco::PointIndex pointIndex(i);
                const draco::AttributeValueIndex positionValueIndex = positionAttr->mapped_index(pointIndex);
                if (positionValueIndex < 0) {
                    spdlog::warn("Decoded Draco POSITION has invalid mapped index at point {}", i);
                    return false;
                }

                std::array<float, 3> pos{0.0f, 0.0f, 0.0f};
                if (!positionAttr->ConvertValue<float, 3>(positionValueIndex, pos.data())) {
                    spdlog::warn("Failed to decode Draco POSITION at vertex {}", i);
                    return false;
                }

                std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
                if (normalAttr && normalAttr->num_components() >= 3) {
                    const draco::AttributeValueIndex normalValueIndex = normalAttr->mapped_index(pointIndex);
                    if (normalValueIndex >= 0) {
                        normalAttr->ConvertValue<float, 3>(normalValueIndex, normal.data());
                    }
                }

                std::array<float, 2> uv{0.0f, 0.0f};
                if (uvAttr && uvAttr->num_components() >= 2) {
                    const draco::AttributeValueIndex uvValueIndex = uvAttr->mapped_index(pointIndex);
                    if (uvValueIndex >= 0) {
                        uvAttr->ConvertValue<float, 2>(uvValueIndex, uv.data());
                        uv[1] = 1.0f - uv[1];
                    }
                }

                std::array<float, 2> uv1{uv[0], uv[1]};
                if (uv1Attr && uv1Attr->num_components() >= 2) {
                    const draco::AttributeValueIndex uv1ValueIndex = uv1Attr->mapped_index(pointIndex);
                    if (uv1ValueIndex >= 0) {
                        uv1Attr->ConvertValue<float, 2>(uv1ValueIndex, uv1.data());
                        uv1[1] = 1.0f - uv1[1];
                    }
                }

                std::array<float, 4> tangent{0.0f, 0.0f, 0.0f, 1.0f};
                if (tangentAttr && tangentAttr->num_components() >= 4) {
                    const draco::AttributeValueIndex tangentValueIndex = tangentAttr->mapped_index(pointIndex);
                    if (tangentValueIndex >= 0) {
                        tangentAttr->ConvertValue<float, 4>(tangentValueIndex, tangent.data());
                        // Flip tangent handedness when flipping V.
                        tangent[3] = -tangent[3];
                    }
                }

                outVertices[static_cast<size_t>(i)] = PackedVertex{
                    pos[0], pos[1], pos[2],
                    normal[0], normal[1], normal[2],
                    uv[0], uv[1],
                    tangent[0], tangent[1], tangent[2], tangent[3],
                    uv1[0], uv1[1]
                };

                const Vector3 position = Vector3::load(pos.data());
                outMinPos = Vector3::min(outMinPos, position);
                outMaxPos = Vector3::max(outMaxPos, position);
            }

            outIndices.clear();
            outIndices.reserve(static_cast<size_t>(dracoMesh->num_faces()) * 3);
            for (draco::FaceIndex faceIndex(0); faceIndex < dracoMesh->num_faces(); ++faceIndex) {
                const auto& face = dracoMesh->face(faceIndex);
                outIndices.push_back(face[0].value());
                outIndices.push_back(face[1].value());
                outIndices.push_back(face[2].value());
            }

            if (!tangentAttr && primitive.mode == TINYGLTF_MODE_TRIANGLES) {
                generateTangents(outVertices, outIndices.empty() ? nullptr : &outIndices);
            }

            return true;
        }

        PrimitiveType mapPrimitiveType(const int mode)
        {
            switch (mode) {
            case TINYGLTF_MODE_POINTS:
                return PRIMITIVE_POINTS;
            case TINYGLTF_MODE_LINE:
                return PRIMITIVE_LINES;
            case TINYGLTF_MODE_LINE_LOOP:
                return PRIMITIVE_LINELOOP;
            case TINYGLTF_MODE_LINE_STRIP:
                return PRIMITIVE_LINESTRIP;
            case TINYGLTF_MODE_TRIANGLE_STRIP:
                return PRIMITIVE_TRISTRIP;
            case TINYGLTF_MODE_TRIANGLE_FAN:
                return PRIMITIVE_TRIFAN;
            case TINYGLTF_MODE_TRIANGLES:
            default:
                return PRIMITIVE_TRIANGLES;
            }
        }

        FilterMode mapMinFilter(const int minFilter)
        {
            switch (minFilter) {
            case 9728: // NEAREST
                return FilterMode::FILTER_NEAREST;
            case 9729: // LINEAR
                return FilterMode::FILTER_LINEAR;
            case 9984: // NEAREST_MIPMAP_NEAREST
                return FilterMode::FILTER_NEAREST_MIPMAP_NEAREST;
            case 9985: // LINEAR_MIPMAP_NEAREST
                return FilterMode::FILTER_LINEAR_MIPMAP_NEAREST;
            case 9986: // NEAREST_MIPMAP_LINEAR
                return FilterMode::FILTER_NEAREST_MIPMAP_LINEAR;
            case 9987: // LINEAR_MIPMAP_LINEAR
            default:
                return FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
            }
        }

        FilterMode mapMagFilter(const int magFilter)
        {
            switch (magFilter) {
            case 9728: // NEAREST
                return FilterMode::FILTER_NEAREST;
            case 9729: // LINEAR
            default:
                return FilterMode::FILTER_LINEAR;
            }
        }

        AddressMode mapWrapMode(const int wrapMode)
        {
            switch (wrapMode) {
            case 33071: // CLAMP_TO_EDGE
                return ADDRESS_CLAMP_TO_EDGE;
            case 33648: // MIRRORED_REPEAT
                return ADDRESS_MIRRORED_REPEAT;
            case 10497: // REPEAT
            default:
                return ADDRESS_REPEAT;
            }
        }

        bool buildRgba8Image(const tinygltf::Image& image, std::vector<uint8_t>& outRgba)
        {
            if (image.width <= 0 || image.height <= 0 || image.image.empty()) {
                return false;
            }
            if (image.bits != 8 || image.pixel_type != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                return false;
            }
            if (image.component < 1 || image.component > 4) {
                return false;
            }

            const auto pixelCount = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
            outRgba.resize(pixelCount * 4);

            for (size_t i = 0; i < pixelCount; ++i) {
                const auto srcOffset = i * static_cast<size_t>(image.component);
                const auto dstOffset = i * 4;
                const auto* src = image.image.data() + srcOffset;
                auto* dst = outRgba.data() + dstOffset;
                switch (image.component) {
                case 1:
                    dst[0] = src[0];
                    dst[1] = src[0];
                    dst[2] = src[0];
                    dst[3] = 255;
                    break;
                case 2:
                    dst[0] = src[0];
                    dst[1] = src[0];
                    dst[2] = src[0];
                    dst[3] = src[1];
                    break;
                case 3:
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                    break;
                case 4:
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = src[3];
                    break;
                default:
                    return false;
                }
            }

            return true;
        }

        /// True when a glTF image holds raw KTX2 bytes (stored verbatim by loadImageData
        /// for KHR_texture_basisu payloads).
        bool imageHoldsKtx2(const tinygltf::Image& image)
        {
            return Ktx2Transcoder::isKtx2(image.image.data(), image.image.size());
        }

        /// Create a block-compressed Texture from raw KTX2 bytes (KHR_texture_basisu).
        /// Returns nullptr on transcode failure. The caller applies sampler state + upload().
        void decomposeNodeMatrix(const std::vector<double>& matrix, Vector3& outT, Quaternion& outR, Vector3& outS)
        {
            if (matrix.size() != 16) {
                return;
            }

            // glTF matrices are column-major.
            const Vector3 col0(static_cast<float>(matrix[0]), static_cast<float>(matrix[1]), static_cast<float>(matrix[2]));
            const Vector3 col1(static_cast<float>(matrix[4]), static_cast<float>(matrix[5]), static_cast<float>(matrix[6]));
            const Vector3 col2(static_cast<float>(matrix[8]), static_cast<float>(matrix[9]), static_cast<float>(matrix[10]));

            float sx = col0.length();
            const float sy = col1.length();
            const float sz = col2.length();
            if (sx <= 0.0f) sx = 1.0f;

            // Match upstream / Quat.setFromMat4 convention for mirrored transforms:
            // keep rotation right-handed and encode mirror sign into X scale.
            const float det = col0.dot(col1.cross(col2));
            if (det < 0.0f) {
                sx = -sx;
            }

            const Matrix4 trs = Matrix4(
                Vector4(col0, 0.0f),
                Vector4(col1, 0.0f),
                Vector4(col2, 0.0f),
                Vector4(0.0f, 0.0f, 0.0f, 1.0f)
            );
            outR = Quaternion::fromMatrix4(trs).normalized();
            outS = Vector3(sx, sy > 0.0f ? sy : 1.0f, sz > 0.0f ? sz : 1.0f);
            outT = Vector3(
                static_cast<float>(matrix[12]),
                static_cast<float>(matrix[13]),
                static_cast<float>(matrix[14])
            );
        }
    }

    /**
     * Apply KHR_materials_transmission / _ior / _volume / _dispersion to a
     * StandardMaterial. Volume attenuation feeds the Beer-law transmittance;
     * dispersion feeds the per-channel refraction in the dynamic grab path.
     *
     * As upstream's extension handlers do, a material carrying transmission OR volume
     * is made blended and switched to dynamic (grab-pass) refraction: without both it
     * renders in the opaque pass and refracts only the environment. Upstream's
     * BLEND_NORMAL is setTransparent(true), NOT setAlphaMode(BLEND), which would also
     * turn depth writes off where upstream keeps them.
     *
     * DEVIATIONS: `ior` is stored as an IOR where upstream stores 1 / ior (see
     * StandardMaterial::refractionIndex). The attenuation colour is stored LINEAR, as
     * glTF authors it; upstream gamma-encodes it and decodes it again on upload, so
     * the shader sees the same value. KHR_texture_transform on the transmission and
     * thickness textures is not applied: those maps have no transform of their own.
     */
    static void applyVolumeExtensions(const tinygltf::Material& srcMaterial, StandardMaterial* material,
        const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture)
    {
        const auto readNumber = [](const tinygltf::Value& v, const char* key, float fallback) {
            if (v.Has(key)) {
                const auto n = v.Get(key);
                if (n.IsNumber()) return static_cast<float>(n.GetNumberAsDouble());
            }
            return fallback;
        };
        const auto textureIndex = [](const tinygltf::Value& v, const char* key) {
            if (v.Has(key)) {
                const auto t = v.Get(key);
                if (t.IsObject() && t.Has("index")) return t.Get("index").GetNumberAsInt();
            }
            return -1;
        };
        const auto makeRefractive = [material]() {
            material->setTransparent(true);
            material->setUseDynamicRefraction(true);
        };

        if (const auto it = srcMaterial.extensions.find("KHR_materials_transmission");
            it != srcMaterial.extensions.end() && it->second.IsObject()) {
            makeRefractive();
            material->setTransmissionFactor(readNumber(it->second, "transmissionFactor", 0.0f));
            if (const int idx = textureIndex(it->second, "transmissionTexture"); idx >= 0) {
                if (const auto tex = getOrCreateTexture(idx)) {
                    material->setRefractionMap(tex.get());
                    material->setRefractionMapChannel(MapChannel::MAP_CHANNEL_R);
                }
            }
        }

        if (const auto it = srcMaterial.extensions.find("KHR_materials_ior");
            it != srcMaterial.extensions.end() && it->second.IsObject()) {
            material->setRefractionIndex(readNumber(it->second, "ior", 1.5f));
        }

        if (const auto it = srcMaterial.extensions.find("KHR_materials_volume");
            it != srcMaterial.extensions.end() && it->second.IsObject()) {
            makeRefractive();
            material->setThickness(readNumber(it->second, "thicknessFactor", 0.0f));
            if (const int idx = textureIndex(it->second, "thicknessTexture"); idx >= 0) {
                if (const auto tex = getOrCreateTexture(idx)) {
                    material->setThicknessMap(tex.get());
                    material->setThicknessMapChannel(MapChannel::MAP_CHANNEL_G);
                }
            }
            material->setAttenuationDistance(readNumber(it->second, "attenuationDistance", 0.0f));
            if (it->second.Has("attenuationColor")) {
                const auto ac = it->second.Get("attenuationColor");
                if (ac.IsArray() && ac.ArrayLen() >= 3) {
                    material->setAttenuationColor(Color(
                        static_cast<float>(ac.Get(0).GetNumberAsDouble()),
                        static_cast<float>(ac.Get(1).GetNumberAsDouble()),
                        static_cast<float>(ac.Get(2).GetNumberAsDouble()), 1.0f));
                }
            }
        }

        if (const auto it = srcMaterial.extensions.find("KHR_materials_dispersion");
            it != srcMaterial.extensions.end() && it->second.IsObject()) {
            material->setDispersion(readNumber(it->second, "dispersion", 0.0f));
        }
    }

    /**
     * Report glTF extensions the file says it REQUIRES that this parser does not
     * implement. `extensionsRequired` is the asset stating it cannot be loaded
     * correctly without them, so a file listing meshopt compression or an
     * unimplemented material model loads PARTIALLY — geometry missing, or a
     * material silently plain — and every later oddity looks like an engine bug.
     *
     * DEVIATION: upstream's parser does not consult the field at all (only its
     * exporter writes one). It is a warning rather than a refusal because the
     * usual case is one cosmetic extension on an otherwise usable file, and the
     * name in the log is what turns an hour of bisecting into a one-line answer.
     */
    static void warnUnsupportedRequiredExtensions(const tinygltf::Model& model, const std::string& debugName)
    {
        // What the parser actually acts on. The texture-container extensions are
        // here because the parser resolves the image index through them; whether
        // the image PAYLOAD decodes is a separate question the image loader
        // answers with its own per-image warning.
        static const std::set<std::string> supported = {
            "KHR_draco_mesh_compression",
            "KHR_materials_clearcoat",
            "KHR_materials_dispersion",
            "KHR_materials_emissive_strength",
            "KHR_materials_ior",
            "KHR_materials_pbrSpecularGlossiness",
            "KHR_materials_transmission",
            "KHR_materials_unlit",
            "KHR_materials_volume",
            "KHR_mesh_quantization",
            "KHR_texture_basisu",
            "KHR_texture_transform",
            "EXT_texture_avif",
            "EXT_texture_webp",
            "MSFT_texture_dds",
        };

        std::string missing;
        for (const auto& extension : model.extensionsRequired) {
            if (supported.contains(extension)) {
                continue;
            }
            if (!missing.empty()) {
                missing += ", ";
            }
            missing += extension;
        }
        if (!missing.empty()) {
            spdlog::warn("GLB [{}] requires extensions this parser does not implement: {}. "
                "The file will load, but not as its author intended.", debugName, missing);
        }
    }

    /**
     * Apply KHR_materials_emissive_strength: a linear multiplier on the emissive
     * term, which is how a glTF asset asks for an emitter brighter than white.
     * It lands on emissiveIntensity because StandardMaterial::updateUniforms
     * uploads pow(emissive, 2.2) * emissiveIntensity — the factor is already
     * linearised there, so the strength must not be gamma-corrected with it.
     */
    static void applyEmissiveStrength(const tinygltf::Material& srcMaterial, StandardMaterial* material)
    {
        const auto it = srcMaterial.extensions.find("KHR_materials_emissive_strength");
        if (it == srcMaterial.extensions.end() || !it->second.IsObject()) {
            return;
        }
        if (const auto& ext = it->second; ext.Has("emissiveStrength")) {
            if (const auto value = ext.Get("emissiveStrength"); value.IsNumber()) {
                material->setEmissiveIntensity(static_cast<float>(value.GetNumberAsDouble()));
            }
        }
    }

    /**
     * Apply KHR_texture_transform to every texture slot that carries one. The
     * extension lives on the texture INFO, not on the material, so each slot is
     * read separately: base colour, metallic-roughness, normal, occlusion and
     * emissive, which are the five StandardMaterial can transform.
     *
     * DEVIATION from upstream's `extractTextureTransform`, which cannot be copied
     * literally: upstream feeds its shader the glTF UVs unchanged, while this
     * parser flips V into the vertex (v = 1 - v) — so the transform is composed
     * with a flip on both sides and the constants come out different.
     *
     * glTF maps uv by [[sx cosT, sy sinT, gx], [-sx sinT, sy cosT, gy]], where T is
     * the rotation counter-clockwise in radians. Substituting v_engine = 1 - v_gltf
     * on both sides and matching `packTransform`'s
     *     u' = cos(r) tx u - sin(r) ty v + ox
     *     v' = sin(r) tx u + cos(r) ty v + (1 - ty - oy)
     * term by term gives tiling = scale, rotation = +T (upstream negates it), and
     * an offset that picks up the rotation:
     *     ox = gx + sy sinT
     *     oy = gy - sy (1 - cosT)
     * At rotation 0 that is just the raw glTF offset. Verified by rendering a
     * transform against the same transform baked into the mesh UVs; a sign error
     * in the V term is invisible under a pure SCALE, because the two spellings
     * then differ by a whole number of tiles and REPEAT wrapping hides it.
     *
     * These are the STANDARDMATERIAL per-map properties, not the base Material's
     * TextureTransform fields: `StandardMaterial::updateUniforms` pushes its own
     * tiling/offset/rotation into those base fields on every pack, so anything
     * written there directly is overwritten before it reaches the GPU — the same
     * trap as setDiffuse versus setBaseColorFactor.
     *
     * DEVIATION: the extension's own `texCoord` override is ignored, as upstream
     * ignores it. The texture info's texCoord still selects the UV set.
     */
    static void applyTextureTransforms(const tinygltf::Material& srcMaterial, StandardMaterial* material)
    {
        struct Transform
        {
            Vector2 tiling{1.0f, 1.0f};
            Vector2 offset{0.0f, 0.0f};
            float rotation = 0.0f;
        };

        const auto readTransform = [](const tinygltf::ExtensionMap& extensions, Transform& out) -> bool {
            const auto it = extensions.find("KHR_texture_transform");
            if (it == extensions.end() || !it->second.IsObject()) {
                return false;
            }
            const auto& ext = it->second;

            const auto readPair = [&ext](const char* key, float& x, float& y) {
                if (!ext.Has(key)) {
                    return;
                }
                if (const auto array = ext.Get(key); array.IsArray() && array.ArrayLen() >= 2) {
                    if (const auto v0 = array.Get(0); v0.IsNumber()) x = static_cast<float>(v0.GetNumberAsDouble());
                    if (const auto v1 = array.Get(1); v1.IsNumber()) y = static_cast<float>(v1.GetNumberAsDouble());
                }
            };

            float offsetX = 0.0f, offsetY = 0.0f, scaleX = 1.0f, scaleY = 1.0f;
            readPair("offset", offsetX, offsetY);
            readPair("scale", scaleX, scaleY);

            float rotation = 0.0f;
            if (ext.Has("rotation")) {
                if (const auto value = ext.Get("rotation"); value.IsNumber()) {
                    rotation = static_cast<float>(value.GetNumberAsDouble());
                }
            }

            constexpr float RAD_TO_DEG = 180.0f / 3.14159265358979323846f;
            const float sinRotation = std::sin(rotation);
            const float cosRotation = std::cos(rotation);
            out.tiling = Vector2(scaleX, scaleY);
            out.offset = Vector2(offsetX + scaleY * sinRotation,
                offsetY - scaleY * (1.0f - cosRotation));
            out.rotation = rotation * RAD_TO_DEG;
            return true;
        };

        Transform transform;
        const auto& pbr = srcMaterial.pbrMetallicRoughness;
        if (readTransform(pbr.baseColorTexture.extensions, transform)) {
            material->setDiffuseMapTiling(transform.tiling);
            material->setDiffuseMapOffset(transform.offset);
            material->setDiffuseMapRotation(transform.rotation);
        }
        if (readTransform(pbr.metallicRoughnessTexture.extensions, transform)) {
            material->setMetalnessMapTiling(transform.tiling);
            material->setMetalnessMapOffset(transform.offset);
            material->setMetalnessMapRotation(transform.rotation);
        }
        if (readTransform(srcMaterial.normalTexture.extensions, transform)) {
            material->setNormalMapTiling(transform.tiling);
            material->setNormalMapOffset(transform.offset);
            material->setNormalMapRotation(transform.rotation);
        }
        if (readTransform(srcMaterial.occlusionTexture.extensions, transform)) {
            material->setAoMapTiling(transform.tiling);
            material->setAoMapOffset(transform.offset);
            material->setAoMapRotation(transform.rotation);
        }
        if (readTransform(srcMaterial.emissiveTexture.extensions, transform)) {
            material->setEmissiveMapTiling(transform.tiling);
            material->setEmissiveMapOffset(transform.offset);
            material->setEmissiveMapRotation(transform.rotation);
        }
    }

    /**
     * Apply KHR_materials_clearcoat to a StandardMaterial. glTF stores coat
     * roughness while the material stores gloss, so the factor routes through
     * setClearCoatGloss + setClearCoatGlossInvert(true). DEVIATION: the shader
     * samples the intensity/roughness maps from the G channel (no per-map
     * channel selection); glTF puts intensity in R — fine for the common
     * greyscale masks (e.g. ClearCoatTest.glb), wrong for packed RGB masks.
     */
    static void applyClearcoat(
        const tinygltf::Material& srcMaterial,
        StandardMaterial* material,
        const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture)
    {
        const auto it = srcMaterial.extensions.find("KHR_materials_clearcoat");
        if (it == srcMaterial.extensions.end() || !it->second.IsObject()) {
            return;
        }
        const auto& cc = it->second;

        const auto readNumber = [&cc](const char* key, const float fallback) {
            if (cc.Has(key)) {
                const auto n = cc.Get(key);
                if (n.IsNumber()) return static_cast<float>(n.GetNumberAsDouble());
            }
            return fallback;
        };
        const auto textureIndex = [&cc](const char* key) {
            if (cc.Has(key)) {
                const auto t = cc.Get(key);
                if (t.IsObject() && t.Has("index")) return t.Get("index").GetNumberAsInt();
            }
            return -1;
        };

        material->setClearCoat(readNumber("clearcoatFactor", 0.0f));
        material->setClearCoatGloss(readNumber("clearcoatRoughnessFactor", 0.0f));
        material->setClearCoatGlossInvert(true);

        if (const int idx = textureIndex("clearcoatTexture"); idx >= 0) {
            if (const auto tex = getOrCreateTexture(idx)) material->setClearCoatMap(tex.get());
        }
        if (const int idx = textureIndex("clearcoatRoughnessTexture"); idx >= 0) {
            if (const auto tex = getOrCreateTexture(idx)) material->setClearCoatGlossMap(tex.get());
        }
        if (const int idx = textureIndex("clearcoatNormalTexture"); idx >= 0) {
            if (const auto tex = getOrCreateTexture(idx)) material->setClearCoatNormalMap(tex.get());
        }
    }

    /**
     * Apply KHR_materials_pbrSpecularGlossiness extension to a StandardMaterial.
     * Maps diffuseTexture → baseColorTexture and specular/glossiness factors.
     */
    static void applySpecularGlossiness(
        const tinygltf::Material& srcMaterial,
        StandardMaterial* material,
        const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture)
    {
        auto sgIt = srcMaterial.extensions.find("KHR_materials_pbrSpecularGlossiness");
        if (sgIt == srcMaterial.extensions.end() || !sgIt->second.IsObject()) return;

        const auto& sg = sgIt->second;

        // diffuseFactor → baseColorFactor (4th component is alpha/opacity)
        if (sg.Has("diffuseFactor")) {
            auto df = sg.Get("diffuseFactor");
            if (df.IsArray() && df.ArrayLen() >= 3) {
                float alpha = df.ArrayLen() >= 4 ? static_cast<float>(df.Get(3).GetNumberAsDouble()) : 1.0f;
                Color diffColor(
                    static_cast<float>(df.Get(0).IsNumber() ? df.Get(0).GetNumberAsDouble() : 1.0),
                    static_cast<float>(df.Get(1).IsNumber() ? df.Get(1).GetNumberAsDouble() : 1.0),
                    static_cast<float>(df.Get(2).IsNumber() ? df.Get(2).GetNumberAsDouble() : 1.0),
                    alpha);
                material->setBaseColorFactor(diffColor);
                Color gammaColor(diffColor);
                gammaColor.gamma();
                material->setDiffuse(gammaColor);
                material->setOpacity(alpha);
                // Enable transparency if alpha < 1
                if (alpha < 1.0f) {
                    material->setAlphaMode(AlphaMode::BLEND);
                    material->setTransparent(true);
                }
            }
        }

        // diffuseTexture → baseColorTexture
        if (sg.Has("diffuseTexture")) {
            auto dt = sg.Get("diffuseTexture");
            if (dt.IsObject() && dt.Has("index")) {
                int texIdx = dt.Get("index").GetNumberAsInt();
                if (auto tex = getOrCreateTexture(texIdx)) {
                    // Set on BOTH base Material and StandardMaterial paths
                    material->setBaseColorTexture(tex.get());
                    material->setHasBaseColorTexture(true);
                    material->setDiffuseMap(tex.get());
                    if (dt.Has("texCoord"))
                        material->setBaseColorUvSet(dt.Get("texCoord").GetNumberAsInt());
                    // Check pixel data
                    {
                        auto* px = static_cast<const uint8_t*>(tex->getLevel(0));
                        if (px) {
                            uint32_t w = tex->width(), h = tex->height();
                            size_t mid = (static_cast<size_t>(h/2) * w + w/2) * 4;
                            size_t q1 = (static_cast<size_t>(h/4) * w + w/4) * 4;
                            spdlog::info("    specGloss diffuseTex OK: texIdx={}, {}x{}, center=({},{},{},{}), q1=({},{},{},{})",
                                texIdx, w, h, px[mid],px[mid+1],px[mid+2],px[mid+3], px[q1],px[q1+1],px[q1+2],px[q1+3]);
                        } else {
                            spdlog::warn("    specGloss diffuseTex OK but NO pixel data on CPU: texIdx={}, {}x{}", texIdx, tex->width(), tex->height());
                        }
                    }
                } else {
                    spdlog::warn("    specGloss diffuseTex FAILED: texIdx={}", texIdx);
                }
            }
        } else {
            spdlog::info("    specGloss: no diffuseTexture field");
        }

        // The specular workflow, as upstream's KHR_materials_pbrSpecularGlossiness
        // extension sets it up: useMetalness off, `specular` stored in sRGB (the factor
        // is linear, so it is gamma-encoded here and linearised again on upload), and
        // the extension's defaults — white specular, glossiness 1 — when a factor is
        // absent. Leaving the specular black would render no specular at all.
        material->setUseMetalness(false);
        material->setMetalness(0.0f);
        material->setMetallicFactor(0.0f);

        // specularGlossinessTexture: rgb = specular color (sRGB), a = glossiness.
        // Bound at the metal-rough slot (3); the SPEC_GLOSS variant reinterprets it.
        if (sg.Has("specularGlossinessTexture")) {
            auto sgt = sg.Get("specularGlossinessTexture");
            if (sgt.IsObject() && sgt.Has("index")) {
                int texIdx = sgt.Get("index").GetNumberAsInt();
                if (auto tex = getOrCreateTexture(texIdx)) {
                    material->setSpecGlossMap(tex.get());
                }
            }
        }

        Color specular(1.0f, 1.0f, 1.0f, 1.0f);
        if (sg.Has("specularFactor")) {
            auto sf = sg.Get("specularFactor");
            if (sf.IsArray() && sf.ArrayLen() >= 3) {
                specular = Color(
                    static_cast<float>(sf.Get(0).GetNumberAsDouble()),
                    static_cast<float>(sf.Get(1).GetNumberAsDouble()),
                    static_cast<float>(sf.Get(2).GetNumberAsDouble()), 1.0f);
            }
        }
        specular.gamma();
        material->setSpecular(specular);

        float gloss = 1.0f;
        if (sg.Has("glossinessFactor")) {
            auto gf = sg.Get("glossinessFactor");
            if (gf.IsNumber()) {
                gloss = static_cast<float>(gf.GetNumberAsDouble());
            }
        }
        material->setGloss(gloss);
        material->setRoughnessFactor(1.0f - gloss);

        // If material is BLEND but opacity is still 1.0, set a reasonable glass opacity.
        // This handles glass materials that rely on BLEND mode for transparency
        // but don't have an explicit low alpha in diffuseFactor.
        if (material->alphaMode() == AlphaMode::BLEND && material->opacity() >= 1.0f) {
            material->setOpacity(0.15f);
            material->setBaseColorFactor(Color(
                material->baseColorFactor().r,
                material->baseColorFactor().g,
                material->baseColorFactor().b,
                0.15f));
        }
    }

    // The material a primitive with no material gets.
    static std::shared_ptr<StandardMaterial> createDefaultGltfMaterial()
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setName("glTF-default");
        material->setTransparent(false);
        material->setAlphaMode(AlphaMode::OPAQUE);
        material->setMetallicFactor(0.0f);
        material->setRoughnessFactor(1.0f);
        // StandardMaterial scalars always apply, so the default material states them too.
        material->setUseMetalness(true);
        material->setMetalness(0.0f);
        material->setGloss(0.0f);
        material->setShaderVariantKey(1);
        return material;
    }

    // One glTF material, for EVERY load path: the synchronous parse(), createFromModel
    // and createFromPrepared (the two asynchronous ones). Each used to carry its own
    // copy of this, and the two async copies had drifted — no occlusion texture, no
    // emissive texture, no metallic-roughness UV set and no KHR_materials_unlit — so a
    // model loaded with loadAsync lost its baked AO and its glow, and an unlit model
    // came out lit. `textureCount`, when given, counts the core textures bound.
    static std::shared_ptr<StandardMaterial> createGltfMaterial(const tinygltf::Material& srcMaterial,
        const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture, size_t* textureCount = nullptr)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setName(srcMaterial.name.empty() ? "glTF-material" : srcMaterial.name);

        // A core texture that the model names but that could not be created is worth a
        // warning on every path; before, only one async path said so.
        const auto bindTexture = [&](const int textureIndex, const char* what) -> std::shared_ptr<Texture> {
            auto texture = getOrCreateTexture(textureIndex);
            if (texture) {
                if (textureCount) {
                    ++*textureCount;
                }
            } else {
                spdlog::warn("GLB material '{}': {} texture {} could not be created",
                    material->name(), what, textureIndex);
            }
            return texture;
        };

        const auto& pbr = srcMaterial.pbrMetallicRoughness;
        if (pbr.baseColorFactor.size() == 4) {
            const Color baseColor(
                static_cast<float>(pbr.baseColorFactor[0]),
                static_cast<float>(pbr.baseColorFactor[1]),
                static_cast<float>(pbr.baseColorFactor[2]),
                static_cast<float>(pbr.baseColorFactor[3]));
            material->setBaseColorFactor(baseColor);
            // Also set StandardMaterial diffuse + opacity so that updateUniforms() uses
            // the glTF base color (not white default). glTF baseColorFactor is linear;
            // StandardMaterial.diffuse expects sRGB (the shader applies srgbToLinear).
            Color diffuseColor(baseColor);
            diffuseColor.gamma();
            material->setDiffuse(diffuseColor);
            material->setOpacity(baseColor.a);
        }
        const float metallicFactor = static_cast<float>(pbr.metallicFactor);
        const float roughnessFactor = static_cast<float>(pbr.roughnessFactor);
        material->setMetallicFactor(metallicFactor);
        material->setRoughnessFactor(roughnessFactor);
        // StandardMaterial convention: gloss = 1 - roughness (glossInvert=false).
        material->setMetalness(metallicFactor);
        material->setGloss(1.0f - roughnessFactor);
        // glTF metallic-roughness: upstream createMaterial sets useMetalness for every glTF material.
        material->setUseMetalness(true);

        if (!srcMaterial.alphaMode.empty()) {
            if (srcMaterial.alphaMode == "BLEND") {
                material->setAlphaMode(AlphaMode::BLEND);
                material->setTransparent(true);
            } else if (srcMaterial.alphaMode == "MASK") {
                material->setAlphaMode(AlphaMode::MASK);
                material->setTransparent(false);
            } else {
                material->setAlphaMode(AlphaMode::OPAQUE);
                material->setTransparent(false);
            }
        }
        material->setCullMode(srcMaterial.doubleSided ? CullMode::CULLFACE_NONE : CullMode::CULLFACE_BACK);
        material->setAlphaCutoff(static_cast<float>(srcMaterial.alphaCutoff));

        applySpecularGlossiness(srcMaterial, material.get(), getOrCreateTexture);
        applyVolumeExtensions(srcMaterial, material.get(), getOrCreateTexture);
        applyClearcoat(srcMaterial, material.get(), getOrCreateTexture);
        applyEmissiveStrength(srcMaterial, material.get());
        applyTextureTransforms(srcMaterial, material.get());

        if (pbr.baseColorTexture.index >= 0) {
            if (auto texture = bindTexture(pbr.baseColorTexture.index, "baseColor")) {
                material->setBaseColorTexture(texture.get());
                material->setHasBaseColorTexture(true);
                material->setBaseColorUvSet(pbr.baseColorTexture.texCoord);
            }
        }
        if (srcMaterial.normalTexture.index >= 0) {
            if (auto texture = bindTexture(srcMaterial.normalTexture.index, "normal")) {
                material->setNormalTexture(texture.get());
                material->setHasNormalTexture(true);
                material->setNormalUvSet(srcMaterial.normalTexture.texCoord);
            }
            material->setNormalScale(static_cast<float>(srcMaterial.normalTexture.scale));
            material->setBumpiness(static_cast<float>(srcMaterial.normalTexture.scale));
        }
        if (pbr.metallicRoughnessTexture.index >= 0) {
            if (auto texture = bindTexture(pbr.metallicRoughnessTexture.index, "metallicRoughness")) {
                material->setMetallicRoughnessTexture(texture.get());
                material->setHasMetallicRoughnessTexture(true);
                material->setMetallicRoughnessUvSet(pbr.metallicRoughnessTexture.texCoord);
            }
        }
        if (srcMaterial.occlusionTexture.index >= 0) {
            if (auto texture = bindTexture(srcMaterial.occlusionTexture.index, "occlusion")) {
                material->setOcclusionTexture(texture.get());
                material->setHasOcclusionTexture(true);
                material->setOcclusionUvSet(srcMaterial.occlusionTexture.texCoord);
            }
            material->setOcclusionStrength(static_cast<float>(srcMaterial.occlusionTexture.strength));
        }
        if (srcMaterial.emissiveFactor.size() == 3) {
            // glTF emissiveFactor is linear; material.emissive expects sRGB.
            Color emissiveColor(
                static_cast<float>(srcMaterial.emissiveFactor[0]),
                static_cast<float>(srcMaterial.emissiveFactor[1]),
                static_cast<float>(srcMaterial.emissiveFactor[2]),
                1.0f);
            emissiveColor.gamma();
            material->setEmissiveFactor(emissiveColor);
            // StandardMaterial::updateUniforms overrides emissiveFactor with
            // _emissive * _emissiveIntensity — mirror into the authoritative slot.
            material->setEmissive(emissiveColor);
        }
        if (srcMaterial.emissiveTexture.index >= 0) {
            if (auto texture = bindTexture(srcMaterial.emissiveTexture.index, "emissive")) {
                material->setEmissiveTexture(texture.get());
                material->setHasEmissiveTexture(true);
                material->setEmissiveUvSet(srcMaterial.emissiveTexture.texCoord);
            }
        }

        const bool isUnlit = srcMaterial.extensions.contains("KHR_materials_unlit");

        uint64_t variant = 1;
        if (material->hasBaseColorTexture()) {
            variant |= (1ull << 1);
        }
        if (material->hasNormalTexture()) {
            variant |= (1ull << 4);
        }
        if (material->hasMetallicRoughnessTexture()) {
            variant |= (1ull << 5);
        }
        if (material->hasOcclusionTexture()) {
            variant |= (1ull << 6);
        }
        if (material->hasEmissiveTexture()) {
            variant |= (1ull << 7);
        }
        if (material->alphaMode() == AlphaMode::BLEND) {
            variant |= (1ull << 2);
        } else if (material->alphaMode() == AlphaMode::MASK) {
            variant |= (1ull << 3);
        }
        if (isUnlit) {
            variant |= (1ull << 32);  // VT_FEATURE_UNLIT
        }
        material->setShaderVariantKey(variant);
        return material;
    }

    // ── Shared building blocks of the one load pipeline ──────────────────
    //
    // Every load path ends in prepareFromModel() + createFromPrepared(): parse() and
    // parseFromMemory() load a model and call createFromModel(), which runs both
    // halves on the calling thread, and the asset loader runs the first half on a
    // worker. The three paths used to carry their own copies of texture creation,
    // vertex extraction and node building; the async copies had drifted (no point
    // clouds, no material features, fewer warnings), which nothing noticed because
    // no example loads asynchronously.

    namespace
    {
        const tinygltf::Accessor* primitiveAttribute(const tinygltf::Model& model,
            const tinygltf::Primitive& primitive, const char* name)
        {
            const auto it = primitive.attributes.find(name);
            return it != primitive.attributes.end() ? getAccessor(model, it->second) : nullptr;
        }

        // A quantised POSITION is still a POSITION: the readers de-quantise every
        // component type glTF allows, so only an unreadable one is grounds for
        // dropping the primitive. Requiring float here dropped each one whole and
        // silently.
        const tinygltf::Accessor* readablePositions(const tinygltf::Model& model, const tinygltf::Primitive& primitive)
        {
            const auto* positions = primitiveAttribute(model, primitive, "POSITION");
            if (!positions || positions->count <= 0 || positions->type != TINYGLTF_TYPE_VEC3 ||
                componentBytes(positions->componentType) <= 0) {
                return nullptr;
            }
            return positions;
        }

        // The image a texture samples: its `source`, or the `source` of one of the
        // extensions that store it there instead.
        int textureImageSource(const tinygltf::Texture& texture)
        {
            if (texture.source >= 0) {
                return texture.source;
            }
            static const char* textureExtensions[] = {
                "KHR_texture_basisu", "EXT_texture_webp", "EXT_texture_avif", "MSFT_texture_dds"
            };
            for (const auto* extName : textureExtensions) {
                const auto it = texture.extensions.find(extName);
                if (it != texture.extensions.end() && it->second.IsObject()) {
                    const auto sourceVal = it->second.Get("source");
                    if (sourceVal.IsInt()) {
                        return sourceVal.GetNumberAsInt();
                    }
                }
            }
            return -1;
        }

        void applyGltfSampler(Texture& texture, const tinygltf::Model& model, const tinygltf::Texture& srcTexture)
        {
            if (srcTexture.sampler < 0 || srcTexture.sampler >= static_cast<int>(model.samplers.size())) {
                return;
            }
            const auto& sampler = model.samplers[static_cast<size_t>(srcTexture.sampler)];
            if (sampler.minFilter != -1) {
                auto minFilter = mapMinFilter(sampler.minFilter);
                if (minFilter == FilterMode::FILTER_NEAREST_MIPMAP_NEAREST ||
                    minFilter == FilterMode::FILTER_LINEAR_MIPMAP_NEAREST ||
                    minFilter == FilterMode::FILTER_NEAREST_MIPMAP_LINEAR ||
                    minFilter == FilterMode::FILTER_LINEAR_MIPMAP_LINEAR) {
                    minFilter = FilterMode::FILTER_LINEAR;
                }
                texture.setMinFilter(minFilter);
            }
            if (sampler.magFilter != -1) {
                texture.setMagFilter(mapMagFilter(sampler.magFilter));
            }
            texture.setAddressU(mapWrapMode(sampler.wrapS));
            texture.setAddressV(mapWrapMode(sampler.wrapT));
        }

        std::shared_ptr<Texture> createPreparedTexture(const PreparedGlbData::ImageData& image,
            const std::string& name, const std::shared_ptr<GraphicsDevice>& device)
        {
            TextureOptions options;
            options.profilerHint = TexHint::TEXHINT_ASSET;
            options.width = static_cast<uint32_t>(image.width);
            options.height = static_cast<uint32_t>(image.height);
            if (image.isCompressed) {
                // KHR_texture_basisu: pre-transcoded block-compressed levels.
                options.format = static_cast<PixelFormat>(image.compressedFormat);
                options.mipmaps = image.compressedLevels.size() > 1;
                options.numLevels = static_cast<uint32_t>(image.compressedLevels.size());
                options.minFilter = options.mipmaps ? FilterMode::FILTER_LINEAR_MIPMAP_LINEAR
                                                    : FilterMode::FILTER_LINEAR;
            } else {
                options.format = PixelFormat::PIXELFORMAT_RGBA8;
                // Allocate a full mip chain — the Metal backend generates levels 1..N via a blit
                // pass after the CPU uploads level 0. Without mipmaps, trilinear/anisotropic
                // sampling can't minify ground/wall textures at glancing angles and we get radial
                // streak aliasing from the viewer's nadir point.
                options.mipmaps = true;
                options.numLevels = 0;  // 0 = allocate full mip chain based on max(w,h)
                options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
            }
            options.magFilter = FilterMode::FILTER_LINEAR;
            options.name = name;

            auto texture = std::make_shared<Texture>(device.get(), options);
            if (image.isCompressed) {
                for (size_t level = 0; level < image.compressedLevels.size(); ++level) {
                    texture->setLevelData(static_cast<uint32_t>(level),
                        image.compressedLevels[level].data(), image.compressedLevels[level].size());
                }
            } else {
                texture->setLevelData(0, image.rgbaPixels.data(), image.rgbaPixels.size());
            }
            return texture;
        }

        // World matrix of every node, for baking a static point cloud into world space.
        std::vector<Matrix4> computeNodeWorldMatrices(const tinygltf::Model& model)
        {
            std::vector<Matrix4> world(model.nodes.size(), Matrix4::identity());

            // 1) Each node's local matrix from TRS (or its direct matrix).
            for (size_t i = 0; i < model.nodes.size(); ++i) {
                const auto& node = model.nodes[i];
                if (node.matrix.size() == 16) {
                    // glTF stores matrices in column-major order, as Matrix4 does, so the
                    // sixteen values are the four columns in sequence. This used to go through
                    // setElement with (row, col) swapped, which wrote the matrix TRANSPOSED;
                    // parseSkins builds the inverse bind matrices this same way.
                    const auto& m = node.matrix;
                    world[i] = Matrix4(
                        Vector4(static_cast<float>(m[0]), static_cast<float>(m[1]), static_cast<float>(m[2]), static_cast<float>(m[3])),
                        Vector4(static_cast<float>(m[4]), static_cast<float>(m[5]), static_cast<float>(m[6]), static_cast<float>(m[7])),
                        Vector4(static_cast<float>(m[8]), static_cast<float>(m[9]), static_cast<float>(m[10]), static_cast<float>(m[11])),
                        Vector4(static_cast<float>(m[12]), static_cast<float>(m[13]), static_cast<float>(m[14]), static_cast<float>(m[15])));
                    continue;
                }
                Vector3 t(0.0f, 0.0f, 0.0f);
                Quaternion q(0.0f, 0.0f, 0.0f, 1.0f);
                Vector3 s(1.0f, 1.0f, 1.0f);
                if (node.translation.size() == 3) {
                    t = Vector3(static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]),
                        static_cast<float>(node.translation[2]));
                }
                if (node.rotation.size() == 4) {
                    q = Quaternion(static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]),
                        static_cast<float>(node.rotation[2]), static_cast<float>(node.rotation[3])).normalized();
                }
                if (node.scale.size() == 3) {
                    s = Vector3(static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]),
                        static_cast<float>(node.scale[2]));
                }
                world[i] = Matrix4::trs(t, q, s);
            }

            // 2) Propagate world = parent_world * local, breadth first from the nodes
            //    that are nobody's child.
            std::vector<bool> isChild(model.nodes.size(), false);
            for (const auto& node : model.nodes) {
                for (const int childIdx : node.children) {
                    if (childIdx >= 0 && childIdx < static_cast<int>(model.nodes.size())) {
                        isChild[static_cast<size_t>(childIdx)] = true;
                    }
                }
            }
            std::queue<size_t> bfs;
            for (size_t i = 0; i < model.nodes.size(); ++i) {
                if (!isChild[i]) bfs.push(i);
            }
            while (!bfs.empty()) {
                const size_t idx = bfs.front();
                bfs.pop();
                for (const int childIdx : model.nodes[idx].children) {
                    if (childIdx >= 0 && childIdx < static_cast<int>(model.nodes.size())) {
                        const auto ci = static_cast<size_t>(childIdx);
                        world[ci] = world[idx] * world[ci];
                        bfs.push(ci);
                    }
                }
            }
            return world;
        }

        // Positions and COLOR_0 of a POINTS primitive, transformed by `transform` when
        // one is given (the static merge bakes world space; an animated model keeps
        // local space so the node's animation still moves the cloud).
        void appendPointVertices(const tinygltf::Model& model, const tinygltf::Accessor& positions,
            const tinygltf::Accessor* colors, const Matrix4* transform,
            std::vector<PackedPointVertex>& out, Vector3& boundsMin, Vector3& boundsMax)
        {
            const size_t base = out.size();
            const auto count = static_cast<size_t>(positions.count);
            out.resize(base + count);
            for (size_t i = 0; i < count; ++i) {
                Vector3 pos;
                if (!readFloatVec3(model, positions, i, pos)) {
                    continue;
                }
                if (transform) {
                    pos = transform->transformPoint(pos);
                }
                float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
                if (colors) {
                    if (colors->type == TINYGLTF_TYPE_VEC4) {
                        Vector4 color;
                        if (readFloatVec4(model, *colors, i, color)) {
                            cr = color.getX(); cg = color.getY(); cb = color.getZ(); ca = color.getW();
                        }
                    } else if (colors->type == TINYGLTF_TYPE_VEC3) {
                        Vector3 color;
                        if (readFloatVec3(model, *colors, i, color)) {
                            cr = color.getX(); cg = color.getY(); cb = color.getZ();
                        }
                    }
                }
                out[base + i] = PackedPointVertex{pos.getX(), pos.getY(), pos.getZ(), cr, cg, cb, ca};
                boundsMin = Vector3::min(boundsMin, pos);
                boundsMax = Vector3::max(boundsMax, pos);
            }
        }

        PreparedGlbData::PrimitiveData pointCloudData(std::vector<PackedPointVertex>&& vertices,
            const Vector3& boundsMin, const Vector3& boundsMax, const int materialIndex)
        {
            PreparedGlbData::PrimitiveData pd;
            pd.pointCloud = true;
            pd.mode = TINYGLTF_MODE_POINTS;
            pd.materialIndex = materialIndex;
            pd.vertexCount = static_cast<int>(vertices.size());
            pd.drawCount = pd.vertexCount;
            pd.vertexBytes.resize(vertices.size() * sizeof(PackedPointVertex));
            std::memcpy(pd.vertexBytes.data(), vertices.data(), pd.vertexBytes.size());
            pd.boundsMin = boundsMin;
            pd.boundsMax = boundsMax;
            return pd;
        }

        // Vertices, indices, skin attributes and morph targets of one non-POINTS
        // primitive. False when the primitive has nothing drawable.
        bool extractTrianglePrimitive(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
            const tinygltf::Primitive& primitive, const size_t meshIndex, PreparedGlbData& counters,
            PreparedGlbData::PrimitiveData& pd)
        {
            pd.mode = primitive.mode;
            pd.materialIndex = primitive.material;

            std::vector<PackedVertex> vertices;
            std::vector<uint32_t> parsedIndices;
            Vector3 minPos(std::numeric_limits<float>::max());
            Vector3 maxPos(std::numeric_limits<float>::lowest());

            bool decodedDraco = false;
            if (primitiveUsesDraco(primitive)) {
                counters.dracoPrimitiveCount++;
                decodedDraco = decodeDracoPrimitive(model, primitive, vertices, parsedIndices, minPos, maxPos);
                if (!decodedDraco) {
                    counters.dracoDecodeFailureCount++;
                    spdlog::warn("Skipping glTF primitive due to Draco decode failure (mesh={})", meshIndex);
                    return false;
                }
                counters.dracoDecodeSuccessCount++;
            }

            if (!decodedDraco) {
                const auto* positions = readablePositions(model, primitive);
                if (!positions) {
                    return false;
                }
                const auto* normals = primitiveAttribute(model, primitive, "NORMAL");
                const auto* uvs = primitiveAttribute(model, primitive, "TEXCOORD_0");
                const auto* uvs1 = primitiveAttribute(model, primitive, "TEXCOORD_1");
                const auto* tangents = primitiveAttribute(model, primitive, "TANGENT");

                const auto vertexCount = static_cast<size_t>(positions->count);
                vertices.resize(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i) {
                    Vector3 pos;
                    if (!readFloatVec3(model, *positions, i, pos)) {
                        continue;
                    }
                    Vector3 normal(0.0f, 1.0f, 0.0f);
                    if (normals) {
                        Vector3 n;
                        if (readFloatVec3(model, *normals, i, n)) normal = n;
                    }
                    // glTF UVs are authored for GL-style sampling conventions. Texture
                    // sampling here uses a top-left origin, so V is flipped.
                    float u = 0.0f, v = 0.0f;
                    if (uvs) {
                        readFloatVec2(model, *uvs, i, u, v);
                        v = 1.0f - v;
                    }
                    float u1 = u, v1 = v;
                    if (uvs1) {
                        readFloatVec2(model, *uvs1, i, u1, v1);
                        v1 = 1.0f - v1;
                    }
                    // Leave the tangent zero when the file carries none; triangle
                    // primitives get one generated below (generateTangents), and the
                    // shaders skip normal mapping on a degenerate tangent rather than
                    // building a bogus fixed basis. There is no derivative-based TBN
                    // fallback in this port.
                    Vector4 tangent(0.0f, 0.0f, 0.0f, 1.0f);
                    if (tangents) {
                        Vector4 t;
                        if (readFloatVec4(model, *tangents, i, t)) {
                            // V is flipped above, so an imported tangent flips handedness.
                            tangent = Vector4(t.getX(), t.getY(), t.getZ(), -t.getW());
                        }
                    }
                    vertices[i] = PackedVertex{
                        pos.getX(), pos.getY(), pos.getZ(),
                        normal.getX(), normal.getY(), normal.getZ(),
                        u, v,
                        tangent.getX(), tangent.getY(), tangent.getZ(), tangent.getW(),
                        u1, v1
                    };
                    minPos = Vector3::min(minPos, pos);
                    maxPos = Vector3::max(maxPos, pos);
                }

                if (!tangents && primitive.mode == TINYGLTF_MODE_TRIANGLES) {
                    if (primitive.indices >= 0) {
                        if (const auto* indexAccessor = getAccessor(model, primitive.indices)) {
                            readIndices(model, *indexAccessor, parsedIndices);
                        }
                    }
                    generateTangents(vertices, parsedIndices.empty() ? nullptr : &parsedIndices);
                }
            }

            if (vertices.empty()) {
                return false;
            }
            if (!decodedDraco && primitive.indices >= 0 && parsedIndices.empty()) {
                if (const auto* indexAccessor = getAccessor(model, primitive.indices)) {
                    readIndices(model, *indexAccessor, parsedIndices);
                }
            }

            // GPU skinning: JOINTS_0/WEIGHTS_0 present → the 88-byte skinned layout
            // (the Draco path never carries skin attributes here).
            pd.vertexCount = static_cast<int>(vertices.size());
            const auto skinAttributes = decodedDraco
                ? SkinAttributes{} : readSkinAttributes(model, primitive, vertices.size());
            if (skinAttributes.valid) {
                pd.skinned = true;
                pd.vertexBytes = packSkinnedVertices(vertices, skinAttributes);
            } else {
                pd.vertexBytes.resize(vertices.size() * sizeof(PackedVertex));
                std::memcpy(pd.vertexBytes.data(), vertices.data(), pd.vertexBytes.size());
            }

            // Morph targets (skipped for Draco primitives — vertex order differs).
            if (!decodedDraco && !primitive.targets.empty()) {
                pd.morphTargets = readMorphTargets(model, primitive, vertices.size());
                pd.morphInitialWeights.assign(mesh.weights.begin(), mesh.weights.end());
            }

            pd.drawCount = static_cast<int>(vertices.size());
            if (!parsedIndices.empty()) {
                pd.indexBytes.resize(parsedIndices.size() * sizeof(uint32_t));
                std::memcpy(pd.indexBytes.data(), parsedIndices.data(), pd.indexBytes.size());
                pd.drawCount = static_cast<int>(parsedIndices.size());
                pd.indexed = true;
            }
            pd.boundsMin = minPos;
            pd.boundsMax = maxPos;
            return true;
        }

        std::shared_ptr<Mesh> createPreparedMesh(PreparedGlbData::PrimitiveData& pd,
            const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<VertexFormat>& format)
        {
            VertexBufferOptions vbOptions;
            vbOptions.data = std::move(pd.vertexBytes);
            auto vertexBuffer = device->createVertexBuffer(format, pd.vertexCount, vbOptions);
            if (!vertexBuffer) {
                return nullptr;
            }

            std::shared_ptr<IndexBuffer> indexBuffer;
            if (pd.indexed && !pd.indexBytes.empty()) {
                const int indexCount = static_cast<int>(pd.indexBytes.size() / sizeof(uint32_t));
                indexBuffer = device->createIndexBuffer(INDEXFORMAT_UINT32, indexCount, pd.indexBytes);
            }

            auto mesh = std::make_shared<Mesh>();
            mesh->setVertexBuffer(vertexBuffer);
            mesh->setIndexBuffer(indexBuffer, 0);

            Primitive drawPrimitive;
            drawPrimitive.type = pd.pointCloud ? PRIMITIVE_POINTS : mapPrimitiveType(pd.mode);
            drawPrimitive.base = 0;
            drawPrimitive.baseVertex = 0;
            drawPrimitive.count = pd.drawCount;
            drawPrimitive.indexed = pd.indexed && indexBuffer;
            mesh->setPrimitive(drawPrimitive, 0);

            BoundingBox bounds;
            bounds.setCenter((pd.boundsMin + pd.boundsMax) * 0.5f);
            bounds.setHalfExtents((pd.boundsMax - pd.boundsMin) * 0.5f);
            mesh->setAabb(bounds);
            return mesh;
        }

        // A point cloud draws unlit with its vertex colours, from a COPY of its glTF
        // material so the point bits cannot leak onto triangle meshes sharing it. An
        // animated model's clouds glow additively without writing depth; the merged
        // static cloud stays opaque.
        std::shared_ptr<Material> pointCloudMaterial(const std::vector<std::shared_ptr<Material>>& materials,
            const int materialIndex, const bool animated)
        {
            const auto& base = (materialIndex >= 0 && materialIndex < static_cast<int>(materials.size()))
                ? materials[static_cast<size_t>(materialIndex)] : materials.front();
            auto material = std::make_shared<StandardMaterial>(*std::static_pointer_cast<StandardMaterial>(base));
            uint64_t variant = material->shaderVariantKey();
            variant |= (1ull << 21);  // VT_FEATURE_VERTEX_COLORS
            variant |= (1ull << 31);  // VT_FEATURE_POINT_SIZE
            if (animated) {
                variant |= (1ull << 32);  // VT_FEATURE_UNLIT
            }
            material->setShaderVariantKey(variant);
            if (animated) {
                material->setTransparent(true);
                material->setBlendState(std::make_shared<BlendState>(BlendState::additiveBlend()));
                material->setDepthState(std::make_shared<DepthState>(DepthState::noWrite()));
            }
            return material;
        }
    }

    std::unique_ptr<GlbContainerResource> GlbParser::parse(const std::string& path,
        const std::shared_ptr<GraphicsDevice>& device)
    {
        if (!device) {
            spdlog::error("GLB parse failed: graphics device is null");
            return nullptr;
        }

        tinygltf::TinyGLTF loader;
        loader.SetImageLoader(GlbParser::loadImageData, nullptr);
        tinygltf::Model model;
        std::string warn;
        std::string err;

        // Detect file format: .gltf (JSON text) vs .glb (binary)
        bool ok = false;
        const auto dot = path.rfind('.');
        const bool isAscii = (dot != std::string::npos &&
            (path.substr(dot) == ".gltf" || path.substr(dot) == ".GLTF"));
        if (isAscii) {
            ok = loader.LoadASCIIFromFile(&model, &err, &warn, path);
        } else {
            ok = loader.LoadBinaryFromFile(&model, &err, &warn, path);
        }
        if (!warn.empty()) {
            spdlog::warn("GLB parse warning [{}]: {}", path, warn);
        }
        if (!ok) {
            spdlog::error("GLB parse failed [{}]: {}", path, err);
            return nullptr;
        }
        return createFromModel(model, device, path);
    }

    std::unique_ptr<GlbContainerResource> GlbParser::parseFromMemory(
        const std::uint8_t* data, const std::size_t length,
        const std::shared_ptr<GraphicsDevice>& device,
        const std::string& debugName)
    {
        if (!device) {
            spdlog::error("GLB parseFromMemory failed: graphics device is null");
            return nullptr;
        }
        if (!data || length == 0) {
            spdlog::error("GLB parseFromMemory failed [{}]: empty data", debugName);
            return nullptr;
        }

        tinygltf::TinyGLTF loader;
        loader.SetImageLoader(GlbParser::loadImageData, nullptr);
        tinygltf::Model model;
        std::string warn;
        std::string err;
        const bool ok = loader.LoadBinaryFromMemory(
            &model, &err, &warn,
            data, static_cast<unsigned int>(length));
        if (!warn.empty()) {
            spdlog::warn("GLB parse warning [{}]: {}", debugName, warn);
        }
        if (!ok) {
            spdlog::error("GLB parseFromMemory failed [{}]: {}", debugName, err);
            return nullptr;
        }

        return createFromModel(model, device, debugName);
    }

    std::unique_ptr<GlbContainerResource> GlbParser::createFromModel(
        tinygltf::Model& model,
        const std::shared_ptr<GraphicsDevice>& device,
        const std::string& debugName)
    {
        if (!device) {
            spdlog::error("GLB createFromModel failed: graphics device is null");
            return nullptr;
        }
        return createFromPrepared(model,
            prepareFromModel(model, device->preferredCompressedRgbaFormat(), debugName), device, debugName);
    }

    // ── prepareFromModel: the CPU-heavy half, safe on a worker thread ────

    PreparedGlbData GlbParser::prepareFromModel(tinygltf::Model& model,
        const PixelFormat ktx2TargetFormat, const std::string& debugName)
    {
        warnUnsupportedRequiredExtensions(model, debugName.empty() ? "glTF" : debugName);

        PreparedGlbData result;

        // ── Images: RGBA8, or transcoded KTX2 ────────────────────────
        result.images.resize(model.images.size());
        for (size_t i = 0; i < model.images.size(); ++i) {
            auto& img = result.images[i];
            const auto& srcImage = model.images[i];
            if (imageHoldsKtx2(srcImage)) {
                // KHR_texture_basisu: transcode here, off the main thread.
                auto transcoded = Ktx2Transcoder::transcode(srcImage.image.data(),
                    srcImage.image.size(), srcImage.name.empty() ? "ktx2" : srcImage.name,
                    ktx2TargetFormat);
                if (transcoded.valid) {
                    img.isCompressed = true;
                    img.compressedFormat = static_cast<uint32_t>(transcoded.format);
                    img.compressedLevels = std::move(transcoded.levels);
                    img.width = static_cast<int>(transcoded.width);
                    img.height = static_cast<int>(transcoded.height);
                    img.valid = true;
                }
                continue;
            }
            img.valid = buildRgba8Image(srcImage, img.rgbaPixels);
            if (img.valid) {
                img.width  = srcImage.width;
                img.height = srcImage.height;
            } else {
                spdlog::warn("glTF image '{}' unsupported format (bits={}, components={}, pixelType={})",
                    srcImage.name, srcImage.bits, srcImage.component, srcImage.pixel_type);
            }
        }

        // ── Primitives ───────────────────────────────────────────────
        // With animations, POINTS stay per node in local space so animating the node
        // moves the cloud; without, they merge into one world-space draw call.
        result.pointCloudsMerged = model.animations.empty();
        std::vector<Matrix4> nodeWorld;
        std::vector<int> meshToNodeIndex;
        std::vector<PackedPointVertex> mergedPoints;
        Vector3 mergedMin(std::numeric_limits<float>::max());
        Vector3 mergedMax(std::numeric_limits<float>::lowest());
        int mergedMaterialIndex = -1;

        result.meshPrimitives.resize(model.meshes.size());
        for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
            const auto& mesh = model.meshes[meshIndex];
            auto& primResults = result.meshPrimitives[meshIndex];

            for (const auto& primitive : mesh.primitives) {
                if (primitive.mode == TINYGLTF_MODE_POINTS) {
                    const auto* positions = readablePositions(model, primitive);
                    if (!positions) {
                        continue;
                    }
                    const auto* colors = primitiveAttribute(model, primitive, "COLOR_0");
                    if (!result.pointCloudsMerged) {
                        std::vector<PackedPointVertex> points;
                        Vector3 pointsMin(std::numeric_limits<float>::max());
                        Vector3 pointsMax(std::numeric_limits<float>::lowest());
                        appendPointVertices(model, *positions, colors, nullptr, points, pointsMin, pointsMax);
                        primResults.push_back(pointCloudData(std::move(points), pointsMin, pointsMax,
                            primitive.material >= 0 ? primitive.material : 0));
                        continue;
                    }
                    if (nodeWorld.empty()) {
                        nodeWorld = computeNodeWorldMatrices(model);
                        // The last node referencing a mesh places it.
                        meshToNodeIndex.assign(model.meshes.size(), -1);
                        for (size_t i = 0; i < model.nodes.size(); ++i) {
                            const int meshRef = model.nodes[i].mesh;
                            if (meshRef >= 0 && meshRef < static_cast<int>(model.meshes.size())) {
                                meshToNodeIndex[static_cast<size_t>(meshRef)] = static_cast<int>(i);
                            }
                        }
                    }
                    if (mergedMaterialIndex < 0) {
                        mergedMaterialIndex = primitive.material >= 0 ? primitive.material : 0;
                    }
                    const int nodeIndex = meshToNodeIndex[meshIndex];
                    const Matrix4 world = nodeIndex >= 0 ? nodeWorld[static_cast<size_t>(nodeIndex)] : Matrix4::identity();
                    appendPointVertices(model, *positions, colors, &world, mergedPoints, mergedMin, mergedMax);
                    continue;
                }

                PreparedGlbData::PrimitiveData pd;
                if (extractTrianglePrimitive(model, mesh, primitive, meshIndex, result, pd)) {
                    primResults.push_back(std::move(pd));
                }
            }
        }
        if (!mergedPoints.empty()) {
            result.mergedPoints = pointCloudData(std::move(mergedPoints), mergedMin, mergedMax, mergedMaterialIndex);
        }

        // ── Animations ───────────────────────────────────────────────
        parseAnimations(model, result.animTracks);

        return result;
    }

    // ── createFromPrepared: GPU resource creation on the main thread ─────

    std::unique_ptr<GlbContainerResource> GlbParser::createFromPrepared(
        tinygltf::Model& model,
        PreparedGlbData&& prepared,
        const std::shared_ptr<GraphicsDevice>& device,
        const std::string& debugName)
    {
        if (!device) {
            spdlog::error("GLB createFromPrepared failed: graphics device is null");
            return nullptr;
        }

        auto container = std::make_unique<GlbContainerResource>();
        const auto vertexFormat = std::make_shared<VertexFormat>(
            sizeof(PackedVertex), VertexFormat::standardElements(), true, false);
        const auto skinnedVertexFormat = std::make_shared<VertexFormat>(
            static_cast<int>(SKINNED_VERTEX_STRIDE), VertexFormat::skinnedElements(), true, false);
        const auto pointVertexFormat = std::make_shared<VertexFormat>(
            static_cast<int>(sizeof(PackedPointVertex)), VertexFormat::pointElements(), true, false);

        // ── Textures, created on first reference by a material ───────
        std::vector<std::shared_ptr<Texture>> gltfTextures(model.textures.size());
        auto getOrCreateTexture = [&](const int textureIndex) -> std::shared_ptr<Texture> {
            if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size())) {
                return nullptr;
            }
            auto& cached = gltfTextures[static_cast<size_t>(textureIndex)];
            if (cached) {
                return cached;
            }

            const auto& srcTexture = model.textures[static_cast<size_t>(textureIndex)];
            const int imageSource = textureImageSource(srcTexture);
            if (imageSource < 0 || imageSource >= static_cast<int>(prepared.images.size())) {
                std::string extensions;
                for (const auto& [name, value] : srcTexture.extensions) {
                    extensions += (extensions.empty() ? "" : ", ") + name;
                }
                spdlog::warn("glTF texture {} has no valid image source (source={}, extensions: {})",
                    textureIndex, srcTexture.source, extensions.empty() ? "none" : extensions);
                return nullptr;
            }
            const auto& image = prepared.images[static_cast<size_t>(imageSource)];
            if (!image.valid || (image.rgbaPixels.empty() && !image.isCompressed)) {
                return nullptr;   // the prepare half already said why
            }

            const auto& srcImage = model.images[static_cast<size_t>(imageSource)];
            auto texture = createPreparedTexture(image, srcImage.name.empty() ? srcTexture.name : srcImage.name,
                device);
            applyGltfSampler(*texture, model, srcTexture);
            texture->upload();
            container->addOwnedTexture(texture);
            cached = texture;
            return cached;
        };

        // ── Materials ────────────────────────────────────────────────
        std::vector<std::shared_ptr<Material>> gltfMaterials;
        gltfMaterials.reserve(std::max<size_t>(1, model.materials.size()));
        size_t actualTextureCount = 0;
        if (model.materials.empty()) {
            gltfMaterials.push_back(createDefaultGltfMaterial());
        } else {
            for (const auto& srcMaterial : model.materials) {
                gltfMaterials.push_back(createGltfMaterial(srcMaterial, getOrCreateTexture, &actualTextureCount));
            }
        }

        // ── Meshes ───────────────────────────────────────────────────
        std::vector<std::vector<size_t>> meshToPayloadIndices(model.meshes.size());
        size_t nextPayloadIndex = 0;
        for (size_t meshIndex = 0; meshIndex < prepared.meshPrimitives.size(); ++meshIndex) {
            for (auto& pd : prepared.meshPrimitives[meshIndex]) {
                if (pd.vertexBytes.empty()) {
                    continue;
                }
                const auto& format = pd.pointCloud ? pointVertexFormat
                    : pd.skinned ? skinnedVertexFormat : vertexFormat;
                auto mesh = createPreparedMesh(pd, device, format);
                if (!mesh) {
                    spdlog::warn("GLB [{}]: vertex buffer creation failed (mesh {})", debugName, meshIndex);
                    continue;
                }

                GlbMeshPayload payload;
                payload.mesh = mesh;
                if (pd.pointCloud) {
                    payload.material = pointCloudMaterial(gltfMaterials, pd.materialIndex, true);
                    payload.castShadow = false;
                } else {
                    payload.material = (pd.materialIndex >= 0 && pd.materialIndex < static_cast<int>(gltfMaterials.size()))
                        ? gltfMaterials[static_cast<size_t>(pd.materialIndex)] : gltfMaterials.front();
                }
                // Morph deltas were extracted by the prepare half; the GPU buffer is built here.
                if (!pd.morphTargets.empty()) {
                    payload.morph = std::make_shared<Morph>(std::move(pd.morphTargets), pd.vertexCount, device.get());
                    payload.morphInitialWeights = std::move(pd.morphInitialWeights);
                }
                container->addMeshPayload(payload);
                meshToPayloadIndices[meshIndex].push_back(nextPayloadIndex++);
            }
        }

        size_t mergedPointPayloadIndex = SIZE_MAX;
        if (prepared.pointCloudsMerged && prepared.mergedPoints.vertexCount > 0) {
            const int mergedVertexCount = prepared.mergedPoints.vertexCount;
            if (auto mesh = createPreparedMesh(prepared.mergedPoints, device, pointVertexFormat)) {
                GlbMeshPayload payload;
                payload.mesh = mesh;
                payload.material = pointCloudMaterial(gltfMaterials, prepared.mergedPoints.materialIndex, false);
                payload.castShadow = false;  // Points don't cast meaningful shadows.
                container->addMeshPayload(payload);
                mergedPointPayloadIndex = nextPayloadIndex++;
                spdlog::info("GLB merged {} point vertices into 1 draw call (AABB {:.2f}–{:.2f})",
                    mergedVertexCount, prepared.mergedPoints.boundsMin.getX(), prepared.mergedPoints.boundsMax.getX());
            }
        }

        // A leaf node whose mesh was ALL points is fully consumed by the merge: its
        // transform is baked into the merged vertices, so it serves no purpose.
        std::vector<bool> meshFullyConsumed(model.meshes.size(), false);
        if (prepared.pointCloudsMerged) {
            for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
                const auto& primitives = model.meshes[mi].primitives;
                meshFullyConsumed[mi] = !primitives.empty() && std::all_of(primitives.begin(), primitives.end(),
                    [](const tinygltf::Primitive& p) { return p.mode == TINYGLTF_MODE_POINTS; });
            }
        }

        // ── Nodes: the glTF hierarchy and local transforms ───────────
        for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex) {
            const auto& node = model.nodes[nodeIndex];
            GlbNodePayload nodePayload;
            // Never empty: an unnamed node is `node_<index>`, the name its
            // animation channels and skin entries carry too (glbNodeName).
            nodePayload.name = glbNodeName(model, static_cast<int>(nodeIndex));
            if (!node.matrix.empty()) {
                decomposeNodeMatrix(node.matrix, nodePayload.translation, nodePayload.rotation, nodePayload.scale);
            }
            if (node.translation.size() == 3) {
                nodePayload.translation = Vector3(static_cast<float>(node.translation[0]),
                    static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2]));
            }
            if (node.rotation.size() == 4) {
                nodePayload.rotation = Quaternion(static_cast<float>(node.rotation[0]),
                    static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]),
                    static_cast<float>(node.rotation[3])).normalized();
            }
            if (node.scale.size() == 3) {
                nodePayload.scale = Vector3(static_cast<float>(node.scale[0]),
                    static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2]));
            }
            if (node.mesh >= 0 && node.mesh < static_cast<int>(meshToPayloadIndices.size())) {
                const auto& mapped = meshToPayloadIndices[static_cast<size_t>(node.mesh)];
                nodePayload.meshPayloadIndices.insert(nodePayload.meshPayloadIndices.end(), mapped.begin(), mapped.end());
                nodePayload.skip = node.children.empty() && meshFullyConsumed[static_cast<size_t>(node.mesh)];
            }
            nodePayload.skinIndex = node.skin;
            nodePayload.children = node.children;
            container->addNodePayload(nodePayload);
        }

        // The merged cloud hangs off a synthetic root node with an identity transform.
        if (mergedPointPayloadIndex != SIZE_MAX) {
            GlbNodePayload pointNode;
            pointNode.name = "__merged_point_cloud";
            pointNode.meshPayloadIndices.push_back(mergedPointPayloadIndex);
            container->addNodePayload(pointNode);
            container->addRootNodeIndex(static_cast<int>(model.nodes.size()));
        }

        int sceneIndex = model.defaultScene;
        if (sceneIndex < 0 && !model.scenes.empty()) {
            sceneIndex = 0;
        }
        if (sceneIndex >= 0 && sceneIndex < static_cast<int>(model.scenes.size())) {
            for (const auto nodeIndex : model.scenes[static_cast<size_t>(sceneIndex)].nodes) {
                container->addRootNodeIndex(nodeIndex);
            }
        }

        // ── Skins and animations ─────────────────────────────────────
        parseSkins(model, container.get());
        for (auto& [name, track] : prepared.animTracks) {
            container->addAnimTrack(name, track);
        }

        if (prepared.dracoPrimitiveCount > 0) {
            spdlog::info("GLB Draco summary [{}]: primitives={}, decoded={}, failed={}",
                debugName, prepared.dracoPrimitiveCount, prepared.dracoDecodeSuccessCount,
                prepared.dracoDecodeFailureCount);
        }
        spdlog::debug("GLB [{}]: textures={} (bound {}), meshes={}, nodes={}",
            debugName, gltfTextures.size(), actualTextureCount, nextPayloadIndex, model.nodes.size());

        return container;
    }
}
