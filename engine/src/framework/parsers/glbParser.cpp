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
        // Per-thread flip state — safe to call from both main and bg threads.
        stbi_set_flip_vertically_on_load_thread(true);
        stbi_uc* decoded = stbi_load_from_memory(bytes, size, &width, &height, &components, 0);
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

        bool readFloatVec3(const tinygltf::Model& model, const tinygltf::Accessor& accessor, const size_t index, Vector3& out)
        {
            if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                accessor.type != TINYGLTF_TYPE_VEC3 ||
                index >= static_cast<size_t>(accessor.count)) {
                return false;
            }
            const auto* base = getAccessorBase(model, accessor);
            if (!base) {
                return false;
            }
            const auto stride = accessorStride(model, accessor);
            const auto* ptr = reinterpret_cast<const float*>(base + index * static_cast<size_t>(stride));
            out = Vector3(ptr[0], ptr[1], ptr[2]);
            return true;
        }

        bool readFloatVec2(const tinygltf::Model& model, const tinygltf::Accessor& accessor, const size_t index, float& u, float& v)
        {
            if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                accessor.type != TINYGLTF_TYPE_VEC2 ||
                index >= static_cast<size_t>(accessor.count)) {
                return false;
            }
            const auto* base = getAccessorBase(model, accessor);
            if (!base) {
                return false;
            }
            const auto stride = accessorStride(model, accessor);
            const auto* ptr = reinterpret_cast<const float*>(base + index * static_cast<size_t>(stride));
            u = ptr[0];
            v = ptr[1];
            return true;
        }

        bool readFloatVec4(const tinygltf::Model& model, const tinygltf::Accessor& accessor, const size_t index, Vector4& out)
        {
            if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                accessor.type != TINYGLTF_TYPE_VEC4 ||
                index >= static_cast<size_t>(accessor.count)) {
                return false;
            }
            const auto* base = getAccessorBase(model, accessor);
            if (!base) {
                return false;
            }
            const auto stride = accessorStride(model, accessor);
            const auto* ptr = reinterpret_cast<const float*>(base + index * static_cast<size_t>(stride));
            out = Vector4(ptr[0], ptr[1], ptr[2], ptr[3]);
            return true;
        }

        bool readFloatScalar(const tinygltf::Model& model, const tinygltf::Accessor& accessor, const size_t index, float& out)
        {
            if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                accessor.type != TINYGLTF_TYPE_SCALAR ||
                index >= static_cast<size_t>(accessor.count)) {
                return false;
            }
            const auto* base = getAccessorBase(model, accessor);
            if (!base) {
                return false;
            }
            const auto stride = accessorStride(model, accessor);
            const auto* ptr = reinterpret_cast<const float*>(base + index * static_cast<size_t>(stride));
            out = ptr[0];
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
            const auto* idxPtr = viewBytes(sparse.indices.bufferView, sparse.indices.byteOffset,
                sparseCount * static_cast<size_t>(idxBytes));
            const auto* valPtr = viewBytes(sparse.values.bufferView, sparse.values.byteOffset,
                sparseCount * static_cast<size_t>(numComponents) * sizeof(float));
            if (!idxPtr || !valPtr) {
                return false;
            }

            const auto* values = reinterpret_cast<const float*>(valPtr);
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
                for (int c = 0; c < numComponents; ++c) {
                    out[index * static_cast<size_t>(numComponents) + static_cast<size_t>(c)] =
                        values[i * static_cast<size_t>(numComponents) + static_cast<size_t>(c)];
                }
            }
            return true;
        }

        // Read all float data from an accessor into a flat vector.
        // Works for SCALAR, VEC2, VEC3, VEC4 — all written as sequential floats.
        // Supports sparse accessors, including the base-less form (bufferView absent,
        // base = zeros) that morph-target deltas commonly use.
        bool readFloatArray(const tinygltf::Model& model, const tinygltf::Accessor& accessor, std::vector<float>& out)
        {
            if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
                return false;
            }
            const int numComponents = tinygltf::GetNumComponentsInType(accessor.type);
            if (numComponents <= 0) {
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
                    const auto* ptr = reinterpret_cast<const float*>(base + i * static_cast<size_t>(stride));
                    for (int c = 0; c < numComponents; ++c) {
                        out[i * static_cast<size_t>(numComponents) + static_cast<size_t>(c)] = ptr[c];
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
                            ? model.nodes[static_cast<size_t>(jointNodeIndex)].name : std::string());
                }

                payload.skin = std::make_shared<Skin>(std::move(inverseBindPose), std::move(boneNames));

                // Per-bone AABBs over the bind-space vertices each joint influences.
                // Enables frustum culling of skinned meshes: at runtime the world
                // bound is the union of bone AABBs transformed by
                // bone.worldTransform * inverseBind (see MeshInstance::aabb()).
                {
                    const size_t jointCount = skin.joints.size();
                    std::vector<float> mins(jointCount * 3, std::numeric_limits<float>::max());
                    std::vector<float> maxs(jointCount * 3, std::numeric_limits<float>::lowest());
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
                            for (size_t v = 0; v < vertexCount; ++v) {
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
                                    for (int c = 0; c < 3; ++c) {
                                        const float value = positions[v * 3 + static_cast<size_t>(c)];
                                        mins[joint * 3 + static_cast<size_t>(c)] = std::min(mins[joint * 3 + static_cast<size_t>(c)], value);
                                        maxs[joint * 3 + static_cast<size_t>(c)] = std::max(maxs[joint * 3 + static_cast<size_t>(c)], value);
                                    }
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
                        boneAabbs[j].setCenter(
                            (mins[j * 3] + maxs[j * 3]) * 0.5f,
                            (mins[j * 3 + 1] + maxs[j * 3 + 1]) * 0.5f,
                            (mins[j * 3 + 2] + maxs[j * 3 + 2]) * 0.5f);
                        boneAabbs[j].setHalfExtents(
                            (maxs[j * 3] - mins[j * 3]) * 0.5f,
                            (maxs[j * 3 + 1] - mins[j * 3 + 1]) * 0.5f,
                            (maxs[j * 3 + 2] - mins[j * 3 + 2]) * 0.5f);
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

                    // Get target node name.
                    const auto& nodeName = model.nodes[static_cast<size_t>(channel.target_node)].name;
                    if (nodeName.empty()) {
                        continue;  // Can't bind unnamed nodes via DefaultAnimBinder.
                    }

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
        void parseAnimations(const tinygltf::Model& model, GlbContainerResource* container)
        {
            if (!container) return;
            std::unordered_map<std::string, std::shared_ptr<AnimTrack>> tracks;
            parseAnimations(model, tracks);
            for (auto& [name, track] : tracks) {
                container->addAnimTrack(name, track);
            }
        }

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

                outMinPos = Vector3(
                    std::min(outMinPos.getX(), pos[0]),
                    std::min(outMinPos.getY(), pos[1]),
                    std::min(outMinPos.getZ(), pos[2])
                );
                outMaxPos = Vector3(
                    std::max(outMaxPos.getX(), pos[0]),
                    std::max(outMaxPos.getY(), pos[1]),
                    std::max(outMaxPos.getZ(), pos[2])
                );
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
        std::shared_ptr<Texture> createTextureFromKtx2(const std::vector<uint8_t>& ktx2Bytes,
            const std::string& name, const std::shared_ptr<GraphicsDevice>& device)
        {
            auto transcoded = Ktx2Transcoder::transcode(ktx2Bytes.data(), ktx2Bytes.size(), name,
                device->preferredCompressedRgbaFormat());
            if (!transcoded.valid) {
                return nullptr;
            }

            TextureOptions options;
            options.width = transcoded.width;
            options.height = transcoded.height;
            options.format = transcoded.format;
            options.mipmaps = transcoded.levels.size() > 1;
            options.numLevels = static_cast<uint32_t>(transcoded.levels.size());
            options.minFilter = options.mipmaps ? FilterMode::FILTER_LINEAR_MIPMAP_LINEAR
                                                : FilterMode::FILTER_LINEAR;
            options.magFilter = FilterMode::FILTER_LINEAR;
            options.name = name;

            auto texture = std::make_shared<Texture>(device.get(), options);
            for (size_t level = 0; level < transcoded.levels.size(); ++level) {
                texture->setLevelData(static_cast<uint32_t>(level),
                    transcoded.levels[level].data(), transcoded.levels[level].size());
            }
            return texture;
        }

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
            const float det = col0.getX() * (col1.getY() * col2.getZ() - col1.getZ() * col2.getY())
                - col1.getX() * (col0.getY() * col2.getZ() - col0.getZ() * col2.getY())
                + col2.getX() * (col0.getY() * col1.getZ() - col0.getZ() * col1.getY());
            if (det < 0.0f) {
                sx = -sx;
            }

            const Matrix4 trs = Matrix4(
                Vector4(col0.getX(), col0.getY(), col0.getZ(), 0.0f),
                Vector4(col1.getX(), col1.getY(), col1.getZ(), 0.0f),
                Vector4(col2.getX(), col2.getY(), col2.getZ(), 0.0f),
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
     */
    static void applyVolumeExtensions(const tinygltf::Material& srcMaterial, StandardMaterial* material)
    {
        const auto readNumber = [](const tinygltf::Value& v, const char* key, float fallback) {
            if (v.Has(key)) {
                const auto n = v.Get(key);
                if (n.IsNumber()) return static_cast<float>(n.GetNumberAsDouble());
            }
            return fallback;
        };

        if (const auto it = srcMaterial.extensions.find("KHR_materials_transmission");
            it != srcMaterial.extensions.end() && it->second.IsObject()) {
            material->setTransmissionFactor(readNumber(it->second, "transmissionFactor", 0.0f));
        }

        if (const auto it = srcMaterial.extensions.find("KHR_materials_ior");
            it != srcMaterial.extensions.end() && it->second.IsObject()) {
            material->setRefractionIndex(readNumber(it->second, "ior", 1.5f));
        }

        if (const auto it = srcMaterial.extensions.find("KHR_materials_volume");
            it != srcMaterial.extensions.end() && it->second.IsObject()) {
            material->setThickness(readNumber(it->second, "thicknessFactor", 0.0f));
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

        // True spec-gloss shader path (VT_FEATURE_SPEC_GLOSS): the fragment shader
        // consumes specularColor/glossiness directly (F0 = specular color) instead
        // of the metal-rough approximation kept below as fallback state.
        material->setUseSpecGloss(true);

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

        // specularFactor
        if (sg.Has("specularFactor")) {
            auto sf = sg.Get("specularFactor");
            if (sf.IsArray() && sf.ArrayLen() >= 3) {
                const Color specColor(
                    static_cast<float>(sf.Get(0).GetNumberAsDouble()),
                    static_cast<float>(sf.Get(1).GetNumberAsDouble()),
                    static_cast<float>(sf.Get(2).GetNumberAsDouble()), 1.0f);
                material->setSpecular(specColor);
                material->setSpecularColor(specColor);
            }
        }

        // glossinessFactor → spec-gloss glossiness (+ legacy roughness fallback)
        float roughness = 0.5f;
        if (sg.Has("glossinessFactor")) {
            auto gf = sg.Get("glossinessFactor");
            if (gf.IsNumber()) {
                float gloss = static_cast<float>(gf.GetNumberAsDouble());
                material->setGloss(gloss);
                material->setGlossiness(gloss);
                roughness = 1.0f - gloss;
            }
        }

        // Specular-glossiness materials are non-metallic; PBR defaults (metallic=1.0)
        // would kill all diffuse color and show only environment reflections (white).
        material->setUseMetalness(false);
        material->setMetalness(0.0f);
        material->setMetallicFactor(0.0f);
        material->setRoughnessFactor(roughness);

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

        auto container = std::make_unique<GlbContainerResource>();
        auto vertexFormat = std::make_shared<VertexFormat>(
            sizeof(PackedVertex), VertexFormat::standardElements(), true, false);
        auto skinnedVertexFormat = std::make_shared<VertexFormat>(
            static_cast<int>(SKINNED_VERTEX_STRIDE), VertexFormat::skinnedElements(), true, false);
        size_t dracoPrimitiveCount = 0;
        size_t dracoDecodeSuccessCount = 0;
        size_t dracoDecodeFailureCount = 0;

        auto makeDefaultMaterial = []() {
            auto material = std::make_shared<StandardMaterial>();
            material->setName("glTF-default");
            material->setTransparent(false);
            material->setAlphaMode(AlphaMode::OPAQUE);
            material->setMetallicFactor(0.0f);
            material->setRoughnessFactor(1.0f);
            material->setShaderVariantKey(1);
            return material;
        };

        std::vector<std::shared_ptr<Material>> gltfMaterials;
        gltfMaterials.reserve(std::max<size_t>(1, model.materials.size()));
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

            // Resolve image source index — check texture extensions first,
            // then fall back to the standard source field
            int imageSource = srcTexture.source;
            if (imageSource < 0) {
                static const char* textureExtensions[] = {
                    "KHR_texture_basisu", "EXT_texture_webp",
                    "EXT_texture_avif", "MSFT_texture_dds"
                };
                for (const auto* extName : textureExtensions) {
                    auto it = srcTexture.extensions.find(extName);
                    if (it != srcTexture.extensions.end() && it->second.IsObject()) {
                        auto sourceVal = it->second.Get("source");
                        if (sourceVal.IsInt()) {
                            imageSource = sourceVal.GetNumberAsInt();
                            break;
                        }
                    }
                }
            }

            if (imageSource < 0 || imageSource >= static_cast<int>(model.images.size())) {
                spdlog::warn("glTF texture {} has no valid image source (source={}, no basisu fallback)",
                    textureIndex, srcTexture.source);
                return nullptr;
            }

            const auto& srcImage = model.images[static_cast<size_t>(imageSource)];
            const std::string textureName = srcImage.name.empty() ? srcTexture.name : srcImage.name;

            std::shared_ptr<Texture> texture;
            if (imageHoldsKtx2(srcImage)) {
                // KHR_texture_basisu: transcode KTX2 to a GPU compressed format.
                texture = createTextureFromKtx2(srcImage.image, textureName, device);
                if (!texture) {
                    return nullptr;
                }
            } else {
                std::vector<uint8_t> rgbaPixels;
                if (!buildRgba8Image(srcImage, rgbaPixels)) {
                    spdlog::warn("glTF image '{}' unsupported format (bits={}, components={}, pixelType={})",
                        srcImage.name, srcImage.bits, srcImage.component, srcImage.pixel_type);
                    return nullptr;
                }

                TextureOptions options;
                options.width = static_cast<uint32_t>(srcImage.width);
                options.height = static_cast<uint32_t>(srcImage.height);
                options.format = PixelFormat::PIXELFORMAT_RGBA8;
                // Allocate a full mip chain — the Metal backend generates levels 1..N via a blit
                // pass after the CPU uploads level 0. Without mipmaps, trilinear/anisotropic
                // sampling can't minify ground/wall textures at glancing angles and we get radial
                // streak aliasing from the viewer's nadir point.
                options.mipmaps = true;
                options.numLevels = 0;  // 0 = allocate full mip chain based on max(w,h)
                options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
                options.magFilter = FilterMode::FILTER_LINEAR;
                options.name = textureName;

                texture = std::make_shared<Texture>(device.get(), options);
                texture->setLevelData(0, rgbaPixels.data(), rgbaPixels.size());
            }

            if (srcTexture.sampler >= 0 && srcTexture.sampler < static_cast<int>(model.samplers.size())) {
                const auto& sampler = model.samplers[static_cast<size_t>(srcTexture.sampler)];
                if (sampler.minFilter != -1) {
                    auto minFilter = mapMinFilter(sampler.minFilter);
                    if (minFilter == FilterMode::FILTER_NEAREST_MIPMAP_NEAREST ||
                        minFilter == FilterMode::FILTER_LINEAR_MIPMAP_NEAREST ||
                        minFilter == FilterMode::FILTER_NEAREST_MIPMAP_LINEAR ||
                        minFilter == FilterMode::FILTER_LINEAR_MIPMAP_LINEAR) {
                        minFilter = FilterMode::FILTER_LINEAR;
                    }
                    texture->setMinFilter(minFilter);
                }
                if (sampler.magFilter != -1) {
                    texture->setMagFilter(mapMagFilter(sampler.magFilter));
                }
                texture->setAddressU(mapWrapMode(sampler.wrapS));
                texture->setAddressV(mapWrapMode(sampler.wrapT));
            }

            texture->upload();
            container->addOwnedTexture(texture);
            cached = texture;
            return cached;
        };

        if (model.materials.empty()) {
            gltfMaterials.push_back(makeDefaultMaterial());
        } else {
            for (size_t materialIndex = 0; materialIndex < model.materials.size(); ++materialIndex) {
                const auto& srcMaterial = model.materials[materialIndex];
                auto material = std::make_shared<StandardMaterial>();
                material->setName(srcMaterial.name.empty() ? "glTF-material" : srcMaterial.name);

                const auto& pbr = srcMaterial.pbrMetallicRoughness;
                if (pbr.baseColorFactor.size() == 4) {
                    const Color baseColor(
                        static_cast<float>(pbr.baseColorFactor[0]),
                        static_cast<float>(pbr.baseColorFactor[1]),
                        static_cast<float>(pbr.baseColorFactor[2]),
                        static_cast<float>(pbr.baseColorFactor[3])
                    );
                    material->setBaseColorFactor(baseColor);
                    // Also set StandardMaterial diffuse + opacity so that
                    // updateUniforms() uses the glTF base color (not white default).
                    // glTF baseColorFactor is linear; StandardMaterial.diffuse
                    // expects sRGB (the shader applies srgbToLinear). Convert with .gamma().
                    Color diffuseColor(baseColor);
                    diffuseColor.gamma();
                    material->setDiffuse(diffuseColor);
                    material->setOpacity(baseColor.a);
                }
                float metallicFactor = static_cast<float>(pbr.metallicFactor);
                float roughnessFactor = static_cast<float>(pbr.roughnessFactor);
                material->setMetallicFactor(metallicFactor);
                material->setRoughnessFactor(roughnessFactor);
                // Also set StandardMaterial metalness + gloss so that
                // updateUniforms() uses the glTF values (not defaults).
                // StandardMaterial convention: gloss = 1 - roughness (glossInvert=false).
                material->setMetalness(metallicFactor);
                material->setGloss(1.0f - roughnessFactor);

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

                // Handle KHR_materials_pbrSpecularGlossiness extension
                applySpecularGlossiness(srcMaterial, material.get(), getOrCreateTexture);
                applyVolumeExtensions(srcMaterial, material.get());
                applyClearcoat(srcMaterial, material.get(), getOrCreateTexture);

                if (pbr.baseColorTexture.index >= 0) {
                    if (auto baseColorTexture = getOrCreateTexture(pbr.baseColorTexture.index)) {
                        material->setBaseColorTexture(baseColorTexture.get());
                        material->setHasBaseColorTexture(true);
                        material->setBaseColorUvSet(pbr.baseColorTexture.texCoord);
                    }
                }

                if (srcMaterial.normalTexture.index >= 0) {
                    if (auto normalTexture = getOrCreateTexture(srcMaterial.normalTexture.index)) {
                        material->setNormalTexture(normalTexture.get());
                        material->setHasNormalTexture(true);
                        material->setNormalUvSet(srcMaterial.normalTexture.texCoord);
                    }
                    material->setNormalScale(static_cast<float>(srcMaterial.normalTexture.scale));
                }
                if (pbr.metallicRoughnessTexture.index >= 0) {
                    if (auto mrTexture = getOrCreateTexture(pbr.metallicRoughnessTexture.index)) {
                        material->setMetallicRoughnessTexture(mrTexture.get());
                        material->setHasMetallicRoughnessTexture(true);
                        material->setMetallicRoughnessUvSet(pbr.metallicRoughnessTexture.texCoord);
                    }
                }
                if (srcMaterial.occlusionTexture.index >= 0) {
                    if (auto occlusionTexture = getOrCreateTexture(srcMaterial.occlusionTexture.index)) {
                        material->setOcclusionTexture(occlusionTexture.get());
                        material->setHasOcclusionTexture(true);
                        material->setOcclusionUvSet(srcMaterial.occlusionTexture.texCoord);
                    }
                    material->setOcclusionStrength(static_cast<float>(srcMaterial.occlusionTexture.strength));
                }
                if (srcMaterial.emissiveFactor.size() == 3) {
                    // glTF emissiveFactor is linear;
                    // material.emissive expects sRGB. Convert with .gamma().
                    Color emissiveColor(
                        static_cast<float>(srcMaterial.emissiveFactor[0]),
                        static_cast<float>(srcMaterial.emissiveFactor[1]),
                        static_cast<float>(srcMaterial.emissiveFactor[2]),
                        1.0f
                    );
                    emissiveColor.gamma();
                    material->setEmissiveFactor(emissiveColor);
                    // StandardMaterial::updateUniforms overrides emissiveFactor with
                    // _emissive * _emissiveIntensity — mirror into the authoritative slot.
                    material->setEmissive(emissiveColor);
                }
                if (srcMaterial.emissiveTexture.index >= 0) {
                    if (auto emissiveTexture = getOrCreateTexture(srcMaterial.emissiveTexture.index)) {
                        material->setEmissiveTexture(emissiveTexture.get());
                        material->setHasEmissiveTexture(true);
                        material->setEmissiveUvSet(srcMaterial.emissiveTexture.texCoord);
                    }
                }

                // Detect KHR_materials_unlit extension.
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
                gltfMaterials.push_back(material);
            }
        }

        // ---------------------------------------------------------------
        // Pre-compute per-node world matrices so POINTS vertices can be
        // baked into world space when merging into a single draw call.
        // ---------------------------------------------------------------
        std::vector<Matrix4> nodeWorldMatrices(model.nodes.size(), Matrix4::identity());

        // 1) Build each node's local matrix from TRS (or direct matrix).
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            const auto& node = model.nodes[i];
            if (!node.matrix.empty() && node.matrix.size() == 16) {
                // glTF stores matrices in column-major order.
                Matrix4 m;
                for (int col = 0; col < 4; ++col)
                    for (int row = 0; row < 4; ++row)
                        m.setElement(row, col, static_cast<float>(node.matrix[static_cast<size_t>(col * 4 + row)]));
                nodeWorldMatrices[i] = m;
            } else {
                Vector3 t(0.0f, 0.0f, 0.0f);
                Quaternion q(0.0f, 0.0f, 0.0f, 1.0f);
                Vector3 s(1.0f, 1.0f, 1.0f);
                if (node.translation.size() == 3) {
                    t = Vector3(static_cast<float>(node.translation[0]),
                                static_cast<float>(node.translation[1]),
                                static_cast<float>(node.translation[2]));
                }
                if (node.rotation.size() == 4) {
                    q = Quaternion(static_cast<float>(node.rotation[0]),
                                  static_cast<float>(node.rotation[1]),
                                  static_cast<float>(node.rotation[2]),
                                  static_cast<float>(node.rotation[3])).normalized();
                }
                if (node.scale.size() == 3) {
                    s = Vector3(static_cast<float>(node.scale[0]),
                                static_cast<float>(node.scale[1]),
                                static_cast<float>(node.scale[2]));
                }
                // Compose T * R * S (column-major: col c = scale_c * col c of R).
                Matrix4 rotMat = q.toRotationMatrix();
                const float sc[3] = {s.getX(), s.getY(), s.getZ()};
                Matrix4 trs;
                for (int c = 0; c < 3; ++c) {
                    for (int r = 0; r < 3; ++r)
                        trs.setElement(c, r, rotMat.getElement(c, r) * sc[c]);
                    trs.setElement(c, 3, 0.0f);  // row 3 of rotation/scale columns
                }
                // Column 3 = translation.
                trs.setElement(3, 0, t.getX());
                trs.setElement(3, 1, t.getY());
                trs.setElement(3, 2, t.getZ());
                trs.setElement(3, 3, 1.0f);
                nodeWorldMatrices[i] = trs;
            }
        }

        // 2) Propagate: world = parent_world * local.
        //    BFS from root nodes (nodes that are not children of any other node).
        {
            std::vector<bool> isChild(model.nodes.size(), false);
            for (const auto& node : model.nodes) {
                for (int childIdx : node.children) {
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
                for (int childIdx : model.nodes[idx].children) {
                    if (childIdx >= 0 && childIdx < static_cast<int>(model.nodes.size())) {
                        const auto ci = static_cast<size_t>(childIdx);
                        nodeWorldMatrices[ci] = nodeWorldMatrices[idx] * nodeWorldMatrices[ci];
                        bfs.push(ci);
                    }
                }
            }
        }

        // 3) Build mesh→node map (last node referencing the mesh wins).
        std::vector<int> meshToNodeIndex(model.meshes.size(), -1);
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            const int meshRef = model.nodes[i].mesh;
            if (meshRef >= 0 && meshRef < static_cast<int>(model.meshes.size())) {
                meshToNodeIndex[static_cast<size_t>(meshRef)] = static_cast<int>(i);
            }
        }

        // When the GLB contains animations, skip the POINTS merge optimisation.
        // The merge bakes vertex positions into world space and marks leaf nodes
        // as skip, which prevents per-node animation (e.g. scale) from working.
        const bool hasAnimations = !model.animations.empty();

        // Accumulators for merging all POINTS primitives into a single draw call.
        // Individual point meshes are appended here; a single merged mesh payload
        // is created after the mesh loop to avoid per-mesh draw call overhead.
        // Only used when !hasAnimations.
        std::vector<PackedPointVertex> mergedPointVertices;
        Vector3 mergedPtMin(std::numeric_limits<float>::max());
        Vector3 mergedPtMax(std::numeric_limits<float>::lowest());
        int mergedPointMaterialIndex = -1;
        size_t mergedPointPayloadIndex = SIZE_MAX;

        std::vector<std::vector<size_t>> meshToPayloadIndices(model.meshes.size());
        size_t nextPayloadIndex = 0;
        for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
            const auto& mesh = model.meshes[meshIndex];
            for (const auto& primitive : mesh.primitives) {
                // ---------------------------------------------------------------
                // POINTS primitive handling.
                // When no animations: accumulate vertices for merged draw call
                // (transforms baked to world space, single draw call).
                // When animations present: create individual per-node point
                // meshes in local space so scale/transform animation works.
                // ---------------------------------------------------------------
                if (primitive.mode == TINYGLTF_MODE_POINTS) {
                    if (!primitive.attributes.contains("POSITION")) {
                        continue;
                    }
                    const auto* positionAccessor = getAccessor(model, primitive.attributes.at("POSITION"));
                    if (!positionAccessor || positionAccessor->count <= 0) {
                        continue;
                    }
                    if (positionAccessor->componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                        positionAccessor->type != TINYGLTF_TYPE_VEC3) {
                        continue;
                    }

                    const auto* colorAccessor = primitive.attributes.contains("COLOR_0")
                        ? getAccessor(model, primitive.attributes.at("COLOR_0")) : nullptr;

                    const auto pointVertexCount = static_cast<size_t>(positionAccessor->count);

                    if (hasAnimations) {
                        // -------------------------------------------------------
                        // ANIMATED path: individual per-node point mesh (local space).
                        // No transform baking — the Entity hierarchy handles it.
                        // -------------------------------------------------------
                        std::vector<PackedPointVertex> pointVertices(pointVertexCount);
                        Vector3 ptMin(std::numeric_limits<float>::max());
                        Vector3 ptMax(std::numeric_limits<float>::lowest());

                        for (size_t i = 0; i < pointVertexCount; ++i) {
                            Vector3 pos;
                            if (!readFloatVec3(model, *positionAccessor, i, pos)) {
                                continue;
                            }
                            // NO transform baking — keep local-space positions.

                            float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
                            if (colorAccessor) {
                                if (colorAccessor->type == TINYGLTF_TYPE_VEC4) {
                                    Vector4 color;
                                    if (readFloatVec4(model, *colorAccessor, i, color)) {
                                        cr = color.getX(); cg = color.getY();
                                        cb = color.getZ(); ca = color.getW();
                                    }
                                } else if (colorAccessor->type == TINYGLTF_TYPE_VEC3) {
                                    Vector3 color;
                                    if (readFloatVec3(model, *colorAccessor, i, color)) {
                                        cr = color.getX(); cg = color.getY();
                                        cb = color.getZ(); ca = 1.0f;
                                    }
                                }
                            }

                            pointVertices[i] = PackedPointVertex{
                                pos.getX(), pos.getY(), pos.getZ(),
                                cr, cg, cb, ca
                            };

                            ptMin = Vector3(
                                std::min(ptMin.getX(), pos.getX()),
                                std::min(ptMin.getY(), pos.getY()),
                                std::min(ptMin.getZ(), pos.getZ())
                            );
                            ptMax = Vector3(
                                std::max(ptMax.getX(), pos.getX()),
                                std::max(ptMax.getY(), pos.getY()),
                                std::max(ptMax.getZ(), pos.getZ())
                            );
                        }

                        auto pointVertexFormat = std::make_shared<VertexFormat>(
                            static_cast<int>(sizeof(PackedPointVertex)),
                            VertexFormat::pointElements(), true, false);

                        std::vector<uint8_t> pointVertexBytes(pointVertices.size() * sizeof(PackedPointVertex));
                        std::memcpy(pointVertexBytes.data(), pointVertices.data(), pointVertexBytes.size());

                        VertexBufferOptions vbOpts;
                        vbOpts.data = std::move(pointVertexBytes);
                        auto pointVB = device->createVertexBuffer(
                            pointVertexFormat,
                            static_cast<int>(pointVertices.size()),
                            vbOpts);

                        if (pointVB) {
                            auto meshResource = std::make_shared<Mesh>();
                            meshResource->setVertexBuffer(pointVB);

                            Primitive drawPrimitive;
                            drawPrimitive.type = PRIMITIVE_POINTS;
                            drawPrimitive.base = 0;
                            drawPrimitive.baseVertex = 0;
                            drawPrimitive.count = static_cast<int>(pointVertices.size());
                            drawPrimitive.indexed = false;
                            meshResource->setPrimitive(drawPrimitive, 0);

                            BoundingBox bounds;
                            bounds.setCenter((ptMin + ptMax) * 0.5f);
                            bounds.setHalfExtents((ptMax - ptMin) * 0.5f);
                            meshResource->setAabb(bounds);

                            // Clone material with point-specific variant bits.
                            std::shared_ptr<Material> pointMaterial;
                            const int matIdx = primitive.material >= 0 ? primitive.material : 0;
                            if (matIdx < static_cast<int>(gltfMaterials.size())) {
                                pointMaterial = std::make_shared<StandardMaterial>(
                                    *std::static_pointer_cast<StandardMaterial>(
                                        gltfMaterials[static_cast<size_t>(matIdx)]));
                            } else {
                                pointMaterial = std::make_shared<StandardMaterial>(
                                    *std::static_pointer_cast<StandardMaterial>(gltfMaterials.front()));
                            }
                            uint64_t ptVariant = pointMaterial->shaderVariantKey();
                            ptVariant |= (1ull << 21);  // VT_FEATURE_VERTEX_COLORS
                            ptVariant |= (1ull << 31);  // VT_FEATURE_POINT_SIZE
                            ptVariant |= (1ull << 32);  // VT_FEATURE_UNLIT
                            pointMaterial->setShaderVariantKey(ptVariant);

                            // Additive blending for animated point clouds — particles glow and
                            // accumulate light. Disable depth write so particles don't z-fight.
                            pointMaterial->setTransparent(true);
                            pointMaterial->setBlendState(std::make_shared<BlendState>(BlendState::additiveBlend()));
                            pointMaterial->setDepthState(std::make_shared<DepthState>(DepthState::noWrite()));

                            GlbMeshPayload payload;
                            payload.mesh = meshResource;
                            payload.material = pointMaterial;
                            payload.castShadow = false;
                            container->addMeshPayload(payload);
                            meshToPayloadIndices[meshIndex].push_back(nextPayloadIndex++);
                        }
                    } else {
                        // -------------------------------------------------------
                        // STATIC path: accumulate into merged buffer (world space).
                        // -------------------------------------------------------
                        if (mergedPointMaterialIndex < 0) {
                            mergedPointMaterialIndex = primitive.material >= 0 ? primitive.material : 0;
                        }

                        // Look up the world matrix for this mesh's node to bake transforms.
                        const Matrix4 meshWorldMatrix =
                            (meshToNodeIndex[meshIndex] >= 0)
                                ? nodeWorldMatrices[static_cast<size_t>(meshToNodeIndex[meshIndex])]
                                : Matrix4::identity();

                        const size_t baseIndex = mergedPointVertices.size();
                        mergedPointVertices.resize(baseIndex + pointVertexCount);

                        for (size_t i = 0; i < pointVertexCount; ++i) {
                            Vector3 pos;
                            if (!readFloatVec3(model, *positionAccessor, i, pos)) {
                                continue;
                            }
                            // Bake node transform into vertex position (world space).
                            pos = meshWorldMatrix.transformPoint(pos);

                            float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
                            if (colorAccessor) {
                                if (colorAccessor->type == TINYGLTF_TYPE_VEC4) {
                                    Vector4 color;
                                    if (readFloatVec4(model, *colorAccessor, i, color)) {
                                        cr = color.getX(); cg = color.getY();
                                        cb = color.getZ(); ca = color.getW();
                                    }
                                } else if (colorAccessor->type == TINYGLTF_TYPE_VEC3) {
                                    Vector3 color;
                                    if (readFloatVec3(model, *colorAccessor, i, color)) {
                                        cr = color.getX(); cg = color.getY();
                                        cb = color.getZ(); ca = 1.0f;
                                    }
                                }
                            }

                            mergedPointVertices[baseIndex + i] = PackedPointVertex{
                                pos.getX(), pos.getY(), pos.getZ(),
                                cr, cg, cb, ca
                            };

                            mergedPtMin = Vector3(
                                std::min(mergedPtMin.getX(), pos.getX()),
                                std::min(mergedPtMin.getY(), pos.getY()),
                                std::min(mergedPtMin.getZ(), pos.getZ())
                            );
                            mergedPtMax = Vector3(
                                std::max(mergedPtMax.getX(), pos.getX()),
                                std::max(mergedPtMax.getY(), pos.getY()),
                                std::max(mergedPtMax.getZ(), pos.getZ())
                            );
                        }
                    }
                    continue;
                }

                // ---------------------------------------------------------------
                // Non-POINTS primitives: triangles, lines, etc.
                // ---------------------------------------------------------------
                std::vector<PackedVertex> vertices;
                std::vector<uint32_t> parsedIndices;
                Vector3 minPos(std::numeric_limits<float>::max());
                Vector3 maxPos(std::numeric_limits<float>::lowest());

                bool decodedDraco = false;
                if (primitiveUsesDraco(primitive)) {
                    dracoPrimitiveCount++;
                    decodedDraco = decodeDracoPrimitive(model, primitive, vertices, parsedIndices, minPos, maxPos);
                    if (!decodedDraco) {
                        dracoDecodeFailureCount++;
                        spdlog::warn("Skipping glTF primitive due to Draco decode failure (mesh={})", meshIndex);
                        continue;
                    }
                    dracoDecodeSuccessCount++;
                }

                if (!decodedDraco) {
                    if (!primitive.attributes.contains("POSITION")) {
                        continue;
                    }

                    const auto* positionAccessor = getAccessor(model, primitive.attributes.at("POSITION"));
                    if (!positionAccessor || positionAccessor->count <= 0) {
                        continue;
                    }
                    if (positionAccessor->componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
                        positionAccessor->type != TINYGLTF_TYPE_VEC3) {
                        continue;
                    }

                    const auto* normalAccessor = primitive.attributes.contains("NORMAL")
                        ? getAccessor(model, primitive.attributes.at("NORMAL")) : nullptr;
                    const auto* uvAccessor = primitive.attributes.contains("TEXCOORD_0")
                        ? getAccessor(model, primitive.attributes.at("TEXCOORD_0")) : nullptr;
                    const auto* uv1Accessor = primitive.attributes.contains("TEXCOORD_1")
                        ? getAccessor(model, primitive.attributes.at("TEXCOORD_1")) : nullptr;
                    const auto* tangentAccessor = primitive.attributes.contains("TANGENT")
                        ? getAccessor(model, primitive.attributes.at("TANGENT")) : nullptr;

                    const auto vertexCount = static_cast<size_t>(positionAccessor->count);
                    vertices.resize(vertexCount);
                    for (size_t i = 0; i < vertexCount; ++i) {
                        Vector3 pos;
                        if (!readFloatVec3(model, *positionAccessor, i, pos)) {
                            continue;
                        }

                        Vector3 normal(0.0f, 1.0f, 0.0f);
                        if (normalAccessor) {
                            Vector3 n;
                            if (readFloatVec3(model, *normalAccessor, i, n)) {
                                normal = n;
                            }
                        }

                        float u = 0.0f;
                        float v = 0.0f;
                        if (uvAccessor) {
                            readFloatVec2(model, *uvAccessor, i, u, v);
                            // glTF UVs are authored for GL-style sampling conventions.
                            // Metal texture sampling uses top-left origin, so flip V here.
                            v = 1.0f - v;
                        }
                        float u1 = u;
                        float v1 = v;
                        if (uv1Accessor) {
                            readFloatVec2(model, *uv1Accessor, i, u1, v1);
                            v1 = 1.0f - v1;
                        }

                        // Leave the tangent zero when the file carries none; triangle
                        // primitives get one generated below (generateTangents), and the
                        // shaders skip normal mapping on a degenerate tangent rather than
                        // building a bogus fixed basis. There is no derivative-based TBN
                        // fallback in this port.
                        Vector4 tangent(0.0f, 0.0f, 0.0f, 1.0f);
                        if (tangentAccessor) {
                            Vector4 t;
                            if (readFloatVec4(model, *tangentAccessor, i, t)) {
                                // We flip V to match the runtime texture sampling convention.
                                // For imported glTF tangents this requires flipping handedness.
                                t = Vector4(t.getX(), t.getY(), t.getZ(), -t.getW());
                                tangent = t;
                            }
                        }

                        vertices[i] = PackedVertex{
                            pos.getX(), pos.getY(), pos.getZ(),
                            normal.getX(), normal.getY(), normal.getZ(),
                            u, v,
                            tangent.getX(), tangent.getY(), tangent.getZ(), tangent.getW(),
                            u1, v1
                        };

                        minPos = Vector3(
                            std::min(minPos.getX(), pos.getX()),
                            std::min(minPos.getY(), pos.getY()),
                            std::min(minPos.getZ(), pos.getZ())
                        );
                        maxPos = Vector3(
                            std::max(maxPos.getX(), pos.getX()),
                            std::max(maxPos.getY(), pos.getY()),
                            std::max(maxPos.getZ(), pos.getZ())
                        );
                    }

                    if (!tangentAccessor && primitive.mode == TINYGLTF_MODE_TRIANGLES) {
                        if (primitive.indices >= 0) {
                            if (const auto* indexAccessor = getAccessor(model, primitive.indices)) {
                                readIndices(model, *indexAccessor, parsedIndices);
                            }
                        }
                        generateTangents(vertices, parsedIndices.empty() ? nullptr : &parsedIndices);
                    }
                }

                const auto vertexCount = vertices.size();
                if (vertexCount == 0) {
                    continue;
                }

                if (!decodedDraco && primitive.indices >= 0 && parsedIndices.empty()) {
                    if (primitive.indices >= 0) {
                        if (const auto* indexAccessor = getAccessor(model, primitive.indices)) {
                            readIndices(model, *indexAccessor, parsedIndices);
                        }
                    }
                }

                // GPU skinning: JOINTS_0/WEIGHTS_0 present → interleave the 88-byte
                // skinned layout (the Draco path never carries skin attributes here).
                const auto skinAttributes = decodedDraco
                    ? SkinAttributes{} : readSkinAttributes(model, primitive, vertexCount);
                std::vector<uint8_t> vertexBytes;
                if (skinAttributes.valid) {
                    vertexBytes = packSkinnedVertices(vertices, skinAttributes);
                } else {
                    vertexBytes.resize(vertices.size() * sizeof(PackedVertex));
                    std::memcpy(vertexBytes.data(), vertices.data(), vertexBytes.size());
                }
                VertexBufferOptions vbOptions;
                vbOptions.data = std::move(vertexBytes);
                auto vertexBuffer = device->createVertexBuffer(
                    skinAttributes.valid ? skinnedVertexFormat : vertexFormat,
                    static_cast<int>(vertexCount), vbOptions);
                if (!vertexBuffer) {
                    spdlog::warn("GLB parse: vertex buffer creation failed mesh={} primitive", meshIndex);
                    continue;
                }

                std::shared_ptr<IndexBuffer> indexBuffer;
                int drawCount = static_cast<int>(vertexCount);
                bool indexed = false;
                if (primitive.indices >= 0) {
                    if (parsedIndices.empty()) {
                        const auto* indexAccessor = getAccessor(model, primitive.indices);
                        if (indexAccessor) {
                            readIndices(model, *indexAccessor, parsedIndices);
                        }
                    }
                    if (!parsedIndices.empty()) {
                        std::vector<uint8_t> indexBytes(parsedIndices.size() * sizeof(uint32_t));
                        std::memcpy(indexBytes.data(), parsedIndices.data(), indexBytes.size());
                        indexBuffer = device->createIndexBuffer(INDEXFORMAT_UINT32, static_cast<int>(parsedIndices.size()), indexBytes);
                        drawCount = static_cast<int>(parsedIndices.size());
                        indexed = true;
                    }
                }

                auto meshResource = std::make_shared<Mesh>();
                meshResource->setVertexBuffer(vertexBuffer);
                meshResource->setIndexBuffer(indexBuffer, 0);

                Primitive drawPrimitive;
                drawPrimitive.type = mapPrimitiveType(primitive.mode);
                drawPrimitive.base = 0;
                drawPrimitive.baseVertex = 0;
                drawPrimitive.count = drawCount;
                drawPrimitive.indexed = indexed;
                meshResource->setPrimitive(drawPrimitive, 0);

                BoundingBox bounds;
                bounds.setCenter((minPos + maxPos) * 0.5f);
                bounds.setHalfExtents((maxPos - minPos) * 0.5f);
                meshResource->setAabb(bounds);

                GlbMeshPayload payload;
                payload.mesh = meshResource;
                if (primitive.material >= 0 && primitive.material < static_cast<int>(gltfMaterials.size())) {
                    payload.material = gltfMaterials[static_cast<size_t>(primitive.material)];
                } else {
                    payload.material = gltfMaterials.front();
                }
                // Morph targets (skipped for Draco primitives — vertex order differs).
                if (!decodedDraco && !primitive.targets.empty()) {
                    auto morphTargets = readMorphTargets(model, primitive, vertexCount);
                    if (!morphTargets.empty()) {
                        payload.morph = std::make_shared<Morph>(std::move(morphTargets),
                            static_cast<int>(vertexCount), device.get());
                        payload.morphInitialWeights.assign(mesh.weights.begin(), mesh.weights.end());
                    }
                }
                container->addMeshPayload(payload);
                meshToPayloadIndices[meshIndex].push_back(nextPayloadIndex++);
            }
        }

        // ---------------------------------------------------------------
        // Post-loop: create a single merged draw call for all POINTS primitives.
        // Only when the model has no animations (static merge path).
        // ---------------------------------------------------------------
        if (!hasAnimations && !mergedPointVertices.empty()) {
            auto pointVertexFormat = std::make_shared<VertexFormat>(
                static_cast<int>(sizeof(PackedPointVertex)),
                VertexFormat::pointElements(), true, false);

            std::vector<uint8_t> pointVertexBytes(mergedPointVertices.size() * sizeof(PackedPointVertex));
            std::memcpy(pointVertexBytes.data(), mergedPointVertices.data(), pointVertexBytes.size());

            VertexBufferOptions vbOptions;
            vbOptions.data = std::move(pointVertexBytes);
            auto pointVB = device->createVertexBuffer(
                pointVertexFormat,
                static_cast<int>(mergedPointVertices.size()),
                vbOptions);

            if (pointVB) {
                auto meshResource = std::make_shared<Mesh>();
                meshResource->setVertexBuffer(pointVB);

                Primitive drawPrimitive;
                drawPrimitive.type = PRIMITIVE_POINTS;
                drawPrimitive.base = 0;
                drawPrimitive.baseVertex = 0;
                drawPrimitive.count = static_cast<int>(mergedPointVertices.size());
                drawPrimitive.indexed = false;
                meshResource->setPrimitive(drawPrimitive, 0);

                BoundingBox bounds;
                bounds.setCenter((mergedPtMin + mergedPtMax) * 0.5f);
                bounds.setHalfExtents((mergedPtMax - mergedPtMin) * 0.5f);
                meshResource->setAabb(bounds);

                // Clone the material so point-specific bits don't leak to triangle meshes.
                std::shared_ptr<Material> pointMaterial;
                if (mergedPointMaterialIndex >= 0 &&
                    mergedPointMaterialIndex < static_cast<int>(gltfMaterials.size())) {
                    pointMaterial = std::make_shared<StandardMaterial>(
                        *std::static_pointer_cast<StandardMaterial>(
                            gltfMaterials[static_cast<size_t>(mergedPointMaterialIndex)]));
                } else {
                    pointMaterial = std::make_shared<StandardMaterial>(
                        *std::static_pointer_cast<StandardMaterial>(gltfMaterials.front()));
                }

                // Add vertexColors (bit 21) + pointSize (bit 31) to variant key.
                uint64_t ptVariant = pointMaterial->shaderVariantKey();
                ptVariant |= (1ull << 21);  // VT_FEATURE_VERTEX_COLORS
                ptVariant |= (1ull << 31);  // VT_FEATURE_POINT_SIZE
                pointMaterial->setShaderVariantKey(ptVariant);

                GlbMeshPayload payload;
                payload.mesh = meshResource;
                payload.material = pointMaterial;
                payload.castShadow = false;  // Points don't cast meaningful shadows.
                container->addMeshPayload(payload);
                // Remember the payload index for the synthetic node created after glTF nodes.
                mergedPointPayloadIndex = nextPayloadIndex++;

                spdlog::info("GLB merged {} point vertices into 1 draw call (AABB {:.2f}–{:.2f})",
                    mergedPointVertices.size(),
                    mergedPtMin.getX(), mergedPtMax.getX());
            }
        }

        // Identify which meshes are fully consumed by the POINTS merge (all primitives are POINTS).
        // Only relevant when not animated — animated models keep individual entities.
        std::vector<bool> meshFullyConsumed(model.meshes.size(), false);
        if (!hasAnimations) {
            for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
                const auto& m = model.meshes[mi];
                bool allPoints = !m.primitives.empty();
                for (const auto& prim : m.primitives) {
                    if (prim.mode != TINYGLTF_MODE_POINTS) {
                        allPoints = false;
                        break;
                    }
                }
                meshFullyConsumed[mi] = allPoints;
            }
        }

        // Build node payloads preserving glTF hierarchy / local transforms.
        for (const auto& node : model.nodes) {
            GlbNodePayload nodePayload;
            nodePayload.name = node.name;

            if (!node.matrix.empty()) {
                decomposeNodeMatrix(node.matrix, nodePayload.translation, nodePayload.rotation, nodePayload.scale);
            }
            if (node.translation.size() == 3) {
                nodePayload.translation = Vector3(
                    static_cast<float>(node.translation[0]),
                    static_cast<float>(node.translation[1]),
                    static_cast<float>(node.translation[2])
                );
            }
            if (node.rotation.size() == 4) {
                nodePayload.rotation = Quaternion(
                    static_cast<float>(node.rotation[0]),
                    static_cast<float>(node.rotation[1]),
                    static_cast<float>(node.rotation[2]),
                    static_cast<float>(node.rotation[3])
                ).normalized();
            }
            if (node.scale.size() == 3) {
                nodePayload.scale = Vector3(
                    static_cast<float>(node.scale[0]),
                    static_cast<float>(node.scale[1]),
                    static_cast<float>(node.scale[2])
                );
            }

            if (node.mesh >= 0 && node.mesh < static_cast<int>(meshToPayloadIndices.size())) {
                const auto& mapped = meshToPayloadIndices[static_cast<size_t>(node.mesh)];
                nodePayload.meshPayloadIndices.insert(nodePayload.meshPayloadIndices.end(), mapped.begin(), mapped.end());
            }
            nodePayload.skinIndex = node.skin;

            // Skip leaf nodes whose mesh was fully consumed by the POINTS merge.
            // Transforms are already baked into the merged vertex buffer, so these
            // nodes serve no purpose and only add scene graph overhead.
            if (node.children.empty() && node.mesh >= 0 &&
                node.mesh < static_cast<int>(meshFullyConsumed.size()) &&
                meshFullyConsumed[static_cast<size_t>(node.mesh)]) {
                nodePayload.skip = true;
            }

            nodePayload.children = node.children;
            container->addNodePayload(nodePayload);
        }

        // Append synthetic node for the merged point cloud (identity transform).
        if (mergedPointPayloadIndex != SIZE_MAX) {
            GlbNodePayload pointNode;
            pointNode.name = "__merged_point_cloud";
            pointNode.meshPayloadIndices.push_back(mergedPointPayloadIndex);
            container->addNodePayload(pointNode);
            // Add as root so instantiateRenderEntity() picks it up.
            container->addRootNodeIndex(static_cast<int>(model.nodes.size()));
        }

        int sceneIndex = model.defaultScene;
        if (sceneIndex < 0 && !model.scenes.empty()) {
            sceneIndex = 0;
        }
        if (sceneIndex >= 0 && sceneIndex < static_cast<int>(model.scenes.size())) {
            const auto& scene = model.scenes[static_cast<size_t>(sceneIndex)];
            for (const auto nodeIndex : scene.nodes) {
                container->addRootNodeIndex(nodeIndex);
            }
        }

        if (dracoPrimitiveCount > 0) {
            spdlog::info(
                "GLB Draco summary [{}]: primitives={}, decoded={}, failed={}",
                path,
                dracoPrimitiveCount,
                dracoDecodeSuccessCount,
                dracoDecodeFailureCount
            );
        }

        // Parse glTF skins + animations.
        parseSkins(model, container.get());
        parseAnimations(model, container.get());

        return container;
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

    // ── createFromModel: GPU resource creation from pre-parsed model ────

    std::unique_ptr<GlbContainerResource> GlbParser::createFromModel(
        tinygltf::Model& model,
        const std::shared_ptr<GraphicsDevice>& device,
        const std::string& debugName)
    {
        if (!device) {
            spdlog::error("GLB createFromModel failed: graphics device is null");
            return nullptr;
        }

        auto container = std::make_unique<GlbContainerResource>();
        auto vertexFormat = std::make_shared<VertexFormat>(
            sizeof(PackedVertex), VertexFormat::standardElements(), true, false);
        auto skinnedVertexFormat = std::make_shared<VertexFormat>(
            static_cast<int>(SKINNED_VERTEX_STRIDE), VertexFormat::skinnedElements(), true, false);
        size_t dracoPrimitiveCount = 0;
        size_t dracoDecodeSuccessCount = 0;
        size_t dracoDecodeFailureCount = 0;

        auto makeDefaultMaterial = []() {
            auto material = std::make_shared<StandardMaterial>();
            material->setName("glTF-default");
            material->setTransparent(false);
            material->setAlphaMode(AlphaMode::OPAQUE);
            material->setMetallicFactor(0.0f);
            material->setRoughnessFactor(1.0f);
            material->setShaderVariantKey(1);
            return material;
        };

        std::vector<std::shared_ptr<Material>> gltfMaterials;
        gltfMaterials.reserve(std::max<size_t>(1, model.materials.size()));
        std::vector<std::shared_ptr<Texture>> gltfTextures(model.textures.size());

        auto getOrCreateTexture = [&](const int textureIndex) -> std::shared_ptr<Texture> {
            if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size())) {
                return nullptr;
            }
            auto& cached = gltfTextures[static_cast<size_t>(textureIndex)];
            if (cached) return cached;

            const auto& srcTexture = model.textures[static_cast<size_t>(textureIndex)];
            int imageSource = srcTexture.source;
            if (imageSource < 0) {
                static const char* textureExtensions[] = {
                    "KHR_texture_basisu", "EXT_texture_webp",
                    "EXT_texture_avif", "MSFT_texture_dds"
                };
                for (const auto* extName : textureExtensions) {
                    auto it = srcTexture.extensions.find(extName);
                    if (it != srcTexture.extensions.end() && it->second.IsObject()) {
                        auto sourceVal = it->second.Get("source");
                        if (sourceVal.IsInt()) {
                            imageSource = sourceVal.GetNumberAsInt();
                            break;
                        }
                    }
                }
            }
            if (imageSource < 0 || imageSource >= static_cast<int>(model.images.size())) return nullptr;

            const auto& srcImage = model.images[static_cast<size_t>(imageSource)];
            const std::string textureName = srcImage.name.empty() ? srcTexture.name : srcImage.name;

            std::shared_ptr<Texture> texture;
            if (imageHoldsKtx2(srcImage)) {
                // KHR_texture_basisu: transcode KTX2 to a GPU compressed format.
                texture = createTextureFromKtx2(srcImage.image, textureName, device);
                if (!texture) return nullptr;
            } else {
                std::vector<uint8_t> rgbaPixels;
                if (!buildRgba8Image(srcImage, rgbaPixels)) return nullptr;

                TextureOptions options;
                options.width = static_cast<uint32_t>(srcImage.width);
                options.height = static_cast<uint32_t>(srcImage.height);
                options.format = PixelFormat::PIXELFORMAT_RGBA8;
                // Allocate a full mip chain — the Metal backend generates levels 1..N via a blit
                // pass after the CPU uploads level 0. Without mipmaps, trilinear/anisotropic
                // sampling can't minify ground/wall textures at glancing angles and we get radial
                // streak aliasing from the viewer's nadir point.
                options.mipmaps = true;
                options.numLevels = 0;  // 0 = allocate full mip chain based on max(w,h)
                options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
                options.magFilter = FilterMode::FILTER_LINEAR;
                options.name = textureName;

                texture = std::make_shared<Texture>(device.get(), options);
                texture->setLevelData(0, rgbaPixels.data(), rgbaPixels.size());
            }

            if (srcTexture.sampler >= 0 && srcTexture.sampler < static_cast<int>(model.samplers.size())) {
                const auto& sampler = model.samplers[static_cast<size_t>(srcTexture.sampler)];
                if (sampler.minFilter != -1) {
                    auto minFilter = mapMinFilter(sampler.minFilter);
                    if (minFilter == FilterMode::FILTER_NEAREST_MIPMAP_NEAREST ||
                        minFilter == FilterMode::FILTER_LINEAR_MIPMAP_NEAREST ||
                        minFilter == FilterMode::FILTER_NEAREST_MIPMAP_LINEAR ||
                        minFilter == FilterMode::FILTER_LINEAR_MIPMAP_LINEAR) {
                        minFilter = FilterMode::FILTER_LINEAR;
                    }
                    texture->setMinFilter(minFilter);
                }
                if (sampler.magFilter != -1) texture->setMagFilter(mapMagFilter(sampler.magFilter));
                texture->setAddressU(mapWrapMode(sampler.wrapS));
                texture->setAddressV(mapWrapMode(sampler.wrapT));
            }

            texture->upload();
            container->addOwnedTexture(texture);
            cached = texture;
            return cached;
        };

        if (model.materials.empty()) {
            gltfMaterials.push_back(makeDefaultMaterial());
        } else {
            for (size_t materialIndex = 0; materialIndex < model.materials.size(); ++materialIndex) {
                const auto& srcMaterial = model.materials[materialIndex];
                auto material = std::make_shared<StandardMaterial>();
                material->setName(srcMaterial.name.empty() ? "glTF-material" : srcMaterial.name);

                const auto& pbr = srcMaterial.pbrMetallicRoughness;
                if (pbr.baseColorFactor.size() == 4) {
                    const Color baseColor(
                        static_cast<float>(pbr.baseColorFactor[0]),
                        static_cast<float>(pbr.baseColorFactor[1]),
                        static_cast<float>(pbr.baseColorFactor[2]),
                        static_cast<float>(pbr.baseColorFactor[3]));
                    material->setBaseColorFactor(baseColor);
                    Color diffuseColor(baseColor);
                    diffuseColor.gamma();
                    material->setDiffuse(diffuseColor);
                    material->setOpacity(baseColor.a);
                }
                material->setMetallicFactor(static_cast<float>(pbr.metallicFactor));
                material->setRoughnessFactor(static_cast<float>(pbr.roughnessFactor));
                material->setMetalness(static_cast<float>(pbr.metallicFactor));
                material->setGloss(1.0f - static_cast<float>(pbr.roughnessFactor));

                if (!srcMaterial.alphaMode.empty()) {
                    if (srcMaterial.alphaMode == "BLEND") {
                        material->setAlphaMode(AlphaMode::BLEND);
                        material->setTransparent(true);
                    } else if (srcMaterial.alphaMode == "MASK") {
                        material->setAlphaMode(AlphaMode::MASK);
                    } else {
                        material->setAlphaMode(AlphaMode::OPAQUE);
                    }
                }
                material->setCullMode(srcMaterial.doubleSided ? CullMode::CULLFACE_NONE : CullMode::CULLFACE_BACK);
                material->setAlphaCutoff(static_cast<float>(srcMaterial.alphaCutoff));

                // Handle KHR_materials_pbrSpecularGlossiness extension
                applySpecularGlossiness(srcMaterial, material.get(), getOrCreateTexture);
                applyVolumeExtensions(srcMaterial, material.get());
                applyClearcoat(srcMaterial, material.get(), getOrCreateTexture);

                if (pbr.baseColorTexture.index >= 0) {
                    if (auto tex = getOrCreateTexture(pbr.baseColorTexture.index)) {
                        material->setBaseColorTexture(tex.get());
                        material->setHasBaseColorTexture(true);
                        material->setBaseColorUvSet(pbr.baseColorTexture.texCoord);
                    }
                }
                if (srcMaterial.normalTexture.index >= 0) {
                    if (auto tex = getOrCreateTexture(srcMaterial.normalTexture.index)) {
                        material->setNormalTexture(tex.get());
                        material->setHasNormalTexture(true);
                        material->setNormalUvSet(srcMaterial.normalTexture.texCoord);
                    }
                    material->setNormalScale(static_cast<float>(srcMaterial.normalTexture.scale));
                }
                if (pbr.metallicRoughnessTexture.index >= 0) {
                    if (auto tex = getOrCreateTexture(pbr.metallicRoughnessTexture.index)) {
                        material->setMetallicRoughnessTexture(tex.get());
                        material->setHasMetallicRoughnessTexture(true);
                    }
                }
                if (srcMaterial.emissiveFactor.size() == 3) {
                    Color emissiveColor(
                        static_cast<float>(srcMaterial.emissiveFactor[0]),
                        static_cast<float>(srcMaterial.emissiveFactor[1]),
                        static_cast<float>(srcMaterial.emissiveFactor[2]), 1.0f);
                    emissiveColor.gamma();
                    material->setEmissiveFactor(emissiveColor);
                    // StandardMaterial::updateUniforms overrides emissiveFactor with
                    // _emissive * _emissiveIntensity — mirror into the authoritative slot.
                    material->setEmissive(emissiveColor);
                }

                uint64_t variant = 1;
                if (material->hasBaseColorTexture()) variant |= (1ull << 1);
                if (material->hasNormalTexture()) variant |= (1ull << 4);
                if (material->hasMetallicRoughnessTexture()) variant |= (1ull << 5);
                if (material->alphaMode() == AlphaMode::BLEND) variant |= (1ull << 2);
                else if (material->alphaMode() == AlphaMode::MASK) variant |= (1ull << 3);
                material->setShaderVariantKey(variant);
                gltfMaterials.push_back(material);
            }
        }

        std::vector<std::vector<size_t>> meshToPayloadIndices(model.meshes.size());
        size_t nextPayloadIndex = 0;
        for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
            const auto& mesh = model.meshes[meshIndex];
            for (const auto& primitive : mesh.primitives) {
                std::vector<PackedVertex> vertices;
                std::vector<uint32_t> parsedIndices;
                Vector3 minPos(std::numeric_limits<float>::max());
                Vector3 maxPos(std::numeric_limits<float>::lowest());

                bool decodedDraco = false;
                if (primitiveUsesDraco(primitive)) {
                    dracoPrimitiveCount++;
                    decodedDraco = decodeDracoPrimitive(model, primitive, vertices, parsedIndices, minPos, maxPos);
                    if (!decodedDraco) { dracoDecodeFailureCount++; continue; }
                    dracoDecodeSuccessCount++;
                }

                if (!decodedDraco) {
                    if (!primitive.attributes.contains("POSITION")) continue;
                    const auto* posAcc = getAccessor(model, primitive.attributes.at("POSITION"));
                    if (!posAcc || posAcc->count <= 0) continue;
                    if (posAcc->componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || posAcc->type != TINYGLTF_TYPE_VEC3) continue;

                    const auto* normalAcc = primitive.attributes.contains("NORMAL") ? getAccessor(model, primitive.attributes.at("NORMAL")) : nullptr;
                    const auto* uvAcc = primitive.attributes.contains("TEXCOORD_0") ? getAccessor(model, primitive.attributes.at("TEXCOORD_0")) : nullptr;
                    const auto* uv1Acc = primitive.attributes.contains("TEXCOORD_1") ? getAccessor(model, primitive.attributes.at("TEXCOORD_1")) : nullptr;
                    const auto* tanAcc = primitive.attributes.contains("TANGENT") ? getAccessor(model, primitive.attributes.at("TANGENT")) : nullptr;

                    const auto vCount = static_cast<size_t>(posAcc->count);
                    vertices.resize(vCount);
                    for (size_t i = 0; i < vCount; ++i) {
                        Vector3 pos;
                        if (!readFloatVec3(model, *posAcc, i, pos)) continue;
                        Vector3 normal(0.0f, 1.0f, 0.0f);
                        if (normalAcc) { Vector3 n; if (readFloatVec3(model, *normalAcc, i, n)) normal = n; }
                        float u = 0.0f, v = 0.0f;
                        if (uvAcc) { readFloatVec2(model, *uvAcc, i, u, v); v = 1.0f - v; }
                        float u1 = u, v1 = v;
                        if (uv1Acc) { readFloatVec2(model, *uv1Acc, i, u1, v1); v1 = 1.0f - v1; }
                        Vector4 tangent(0.0f, 0.0f, 0.0f, 1.0f);
                        if (tanAcc) { Vector4 t; if (readFloatVec4(model, *tanAcc, i, t)) { tangent = Vector4(t.getX(), t.getY(), t.getZ(), -t.getW()); } }

                        vertices[i] = PackedVertex{
                            pos.getX(), pos.getY(), pos.getZ(),
                            normal.getX(), normal.getY(), normal.getZ(),
                            u, v, tangent.getX(), tangent.getY(), tangent.getZ(), tangent.getW(),
                            u1, v1
                        };
                        minPos = Vector3(std::min(minPos.getX(), pos.getX()), std::min(minPos.getY(), pos.getY()), std::min(minPos.getZ(), pos.getZ()));
                        maxPos = Vector3(std::max(maxPos.getX(), pos.getX()), std::max(maxPos.getY(), pos.getY()), std::max(maxPos.getZ(), pos.getZ()));
                    }
                    if (!tanAcc && primitive.mode == TINYGLTF_MODE_TRIANGLES) {
                        if (primitive.indices >= 0) { if (const auto* ia = getAccessor(model, primitive.indices)) readIndices(model, *ia, parsedIndices); }
                        generateTangents(vertices, parsedIndices.empty() ? nullptr : &parsedIndices);
                    }
                }

                if (vertices.empty()) continue;

                if (!decodedDraco && primitive.indices >= 0 && parsedIndices.empty()) {
                    if (const auto* ia = getAccessor(model, primitive.indices)) readIndices(model, *ia, parsedIndices);
                }

                // GPU skinning: JOINTS_0/WEIGHTS_0 → 88-byte skinned layout.
                const auto skinAttributes = decodedDraco
                    ? SkinAttributes{} : readSkinAttributes(model, primitive, vertices.size());
                std::vector<uint8_t> vertexBytes;
                if (skinAttributes.valid) {
                    vertexBytes = packSkinnedVertices(vertices, skinAttributes);
                } else {
                    vertexBytes.resize(vertices.size() * sizeof(PackedVertex));
                    std::memcpy(vertexBytes.data(), vertices.data(), vertexBytes.size());
                }
                VertexBufferOptions vbOptions;
                vbOptions.data = std::move(vertexBytes);
                auto vb = device->createVertexBuffer(
                    skinAttributes.valid ? skinnedVertexFormat : vertexFormat,
                    static_cast<int>(vertices.size()), vbOptions);
                if (!vb) continue;

                std::shared_ptr<IndexBuffer> ib;
                int drawCount = static_cast<int>(vertices.size());
                bool indexed = false;
                if (!parsedIndices.empty()) {
                    std::vector<uint8_t> indexBytes(parsedIndices.size() * sizeof(uint32_t));
                    std::memcpy(indexBytes.data(), parsedIndices.data(), indexBytes.size());
                    ib = device->createIndexBuffer(INDEXFORMAT_UINT32, static_cast<int>(parsedIndices.size()), indexBytes);
                    drawCount = static_cast<int>(parsedIndices.size());
                    indexed = true;
                }

                auto meshResource = std::make_shared<Mesh>();
                meshResource->setVertexBuffer(vb);
                meshResource->setIndexBuffer(ib, 0);
                Primitive drawPrimitive;
                drawPrimitive.type = mapPrimitiveType(primitive.mode);
                drawPrimitive.base = 0;
                drawPrimitive.baseVertex = 0;
                drawPrimitive.count = drawCount;
                drawPrimitive.indexed = indexed;
                meshResource->setPrimitive(drawPrimitive, 0);

                BoundingBox bounds;
                bounds.setCenter((minPos + maxPos) * 0.5f);
                bounds.setHalfExtents((maxPos - minPos) * 0.5f);
                meshResource->setAabb(bounds);

                GlbMeshPayload payload;
                payload.mesh = meshResource;
                payload.material = (primitive.material >= 0 && primitive.material < static_cast<int>(gltfMaterials.size()))
                    ? gltfMaterials[static_cast<size_t>(primitive.material)] : gltfMaterials.front();
                // Morph targets (skipped for Draco primitives — vertex order differs).
                if (!decodedDraco && !primitive.targets.empty()) {
                    auto morphTargets = readMorphTargets(model, primitive, vertices.size());
                    if (!morphTargets.empty()) {
                        payload.morph = std::make_shared<Morph>(std::move(morphTargets),
                            static_cast<int>(vertices.size()), device.get());
                        payload.morphInitialWeights.assign(mesh.weights.begin(), mesh.weights.end());
                    }
                }
                container->addMeshPayload(payload);
                meshToPayloadIndices[meshIndex].push_back(nextPayloadIndex++);
            }
        }

        for (const auto& node : model.nodes) {
            GlbNodePayload nodePayload;
            nodePayload.name = node.name;
            if (!node.matrix.empty()) decomposeNodeMatrix(node.matrix, nodePayload.translation, nodePayload.rotation, nodePayload.scale);
            if (node.translation.size() == 3) nodePayload.translation = Vector3(static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2]));
            if (node.rotation.size() == 4) nodePayload.rotation = Quaternion(static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]), static_cast<float>(node.rotation[3])).normalized();
            if (node.scale.size() == 3) nodePayload.scale = Vector3(static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2]));
            if (node.mesh >= 0 && node.mesh < static_cast<int>(meshToPayloadIndices.size())) {
                const auto& mapped = meshToPayloadIndices[static_cast<size_t>(node.mesh)];
                nodePayload.meshPayloadIndices.insert(nodePayload.meshPayloadIndices.end(), mapped.begin(), mapped.end());
            }
            nodePayload.skinIndex = node.skin;
            nodePayload.children = node.children;
            container->addNodePayload(nodePayload);
        }

        int sceneIndex = model.defaultScene;
        if (sceneIndex < 0 && !model.scenes.empty()) sceneIndex = 0;
        if (sceneIndex >= 0 && sceneIndex < static_cast<int>(model.scenes.size())) {
            for (const auto nodeIndex : model.scenes[static_cast<size_t>(sceneIndex)].nodes)
                container->addRootNodeIndex(nodeIndex);
        }

        if (dracoPrimitiveCount > 0) {
            spdlog::info("GLB Draco summary [{}]: primitives={}, decoded={}, failed={}",
                debugName, dracoPrimitiveCount, dracoDecodeSuccessCount, dracoDecodeFailureCount);
        }

        // Parse glTF skins + animations.
        parseSkins(model, container.get());
        parseAnimations(model, container.get());

        return container;
    }

    // ── prepareFromModel: CPU-heavy work on background thread ────────

    PreparedGlbData GlbParser::prepareFromModel(tinygltf::Model& model,
        const PixelFormat ktx2TargetFormat)
    {
        PreparedGlbData result;

        // ── Pre-convert all images to RGBA8 ──────────────────────────
        result.images.resize(model.images.size());
        for (size_t i = 0; i < model.images.size(); ++i) {
            auto& img = result.images[i];
            const auto& srcImage = model.images[i];
            if (imageHoldsKtx2(srcImage)) {
                // KHR_texture_basisu: transcode on this background thread.
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
            }
        }

        // ── Pre-extract mesh primitive vertices/indices ──────────────
        result.meshPrimitives.resize(model.meshes.size());
        for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
            const auto& mesh = model.meshes[meshIndex];
            auto& primResults = result.meshPrimitives[meshIndex];

            for (const auto& primitive : mesh.primitives) {
                PreparedGlbData::PrimitiveData pd;
                pd.mode = primitive.mode;
                pd.materialIndex = primitive.material;

                std::vector<PackedVertex> vertices;
                std::vector<uint32_t> parsedIndices;
                Vector3 minPos(std::numeric_limits<float>::max());
                Vector3 maxPos(std::numeric_limits<float>::lowest());

                bool decodedDraco = false;
                if (primitiveUsesDraco(primitive)) {
                    result.dracoPrimitiveCount++;
                    decodedDraco = decodeDracoPrimitive(model, primitive, vertices, parsedIndices, minPos, maxPos);
                    if (!decodedDraco) {
                        result.dracoDecodeFailureCount++;
                        continue;
                    }
                    result.dracoDecodeSuccessCount++;
                }

                if (!decodedDraco) {
                    if (!primitive.attributes.contains("POSITION")) continue;
                    const auto* posAcc = getAccessor(model, primitive.attributes.at("POSITION"));
                    if (!posAcc || posAcc->count <= 0) continue;
                    if (posAcc->componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || posAcc->type != TINYGLTF_TYPE_VEC3) continue;

                    const auto* normalAcc = primitive.attributes.contains("NORMAL") ? getAccessor(model, primitive.attributes.at("NORMAL")) : nullptr;
                    const auto* uvAcc = primitive.attributes.contains("TEXCOORD_0") ? getAccessor(model, primitive.attributes.at("TEXCOORD_0")) : nullptr;
                    const auto* uv1Acc = primitive.attributes.contains("TEXCOORD_1") ? getAccessor(model, primitive.attributes.at("TEXCOORD_1")) : nullptr;
                    const auto* tanAcc = primitive.attributes.contains("TANGENT") ? getAccessor(model, primitive.attributes.at("TANGENT")) : nullptr;

                    const auto vCount = static_cast<size_t>(posAcc->count);
                    vertices.resize(vCount);
                    for (size_t i = 0; i < vCount; ++i) {
                        Vector3 pos;
                        if (!readFloatVec3(model, *posAcc, i, pos)) continue;
                        Vector3 normal(0.0f, 1.0f, 0.0f);
                        if (normalAcc) { Vector3 n; if (readFloatVec3(model, *normalAcc, i, n)) normal = n; }
                        float u = 0.0f, v = 0.0f;
                        if (uvAcc) { readFloatVec2(model, *uvAcc, i, u, v); v = 1.0f - v; }
                        float u1 = u, v1 = v;
                        if (uv1Acc) { readFloatVec2(model, *uv1Acc, i, u1, v1); v1 = 1.0f - v1; }
                        Vector4 tangent(0.0f, 0.0f, 0.0f, 1.0f);
                        if (tanAcc) { Vector4 t; if (readFloatVec4(model, *tanAcc, i, t)) { tangent = Vector4(t.getX(), t.getY(), t.getZ(), -t.getW()); } }

                        vertices[i] = PackedVertex{
                            pos.getX(), pos.getY(), pos.getZ(),
                            normal.getX(), normal.getY(), normal.getZ(),
                            u, v, tangent.getX(), tangent.getY(), tangent.getZ(), tangent.getW(),
                            u1, v1
                        };
                        minPos = Vector3(std::min(minPos.getX(), pos.getX()), std::min(minPos.getY(), pos.getY()), std::min(minPos.getZ(), pos.getZ()));
                        maxPos = Vector3(std::max(maxPos.getX(), pos.getX()), std::max(maxPos.getY(), pos.getY()), std::max(maxPos.getZ(), pos.getZ()));
                    }
                    if (!tanAcc && primitive.mode == TINYGLTF_MODE_TRIANGLES) {
                        if (primitive.indices >= 0) { if (const auto* ia = getAccessor(model, primitive.indices)) readIndices(model, *ia, parsedIndices); }
                        generateTangents(vertices, parsedIndices.empty() ? nullptr : &parsedIndices);
                    }
                }

                if (vertices.empty()) continue;

                if (!decodedDraco && primitive.indices >= 0 && parsedIndices.empty()) {
                    if (const auto* ia = getAccessor(model, primitive.indices)) readIndices(model, *ia, parsedIndices);
                }

                // Pack into byte arrays (88-byte skinned layout when skin attributes exist).
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
                pd.indexed = false;
                if (!parsedIndices.empty()) {
                    pd.indexBytes.resize(parsedIndices.size() * sizeof(uint32_t));
                    std::memcpy(pd.indexBytes.data(), parsedIndices.data(), pd.indexBytes.size());
                    pd.drawCount = static_cast<int>(parsedIndices.size());
                    pd.indexed = true;
                }

                pd.boundsMin = minPos;
                pd.boundsMax = maxPos;
                primResults.push_back(std::move(pd));
            }
        }

        // ── Parse animations ─────────────────────────────────────────
        parseAnimations(model, result.animTracks);

        return result;
    }

    // ── createFromPrepared: fast GPU resource creation on main thread ─

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
        auto vertexFormat = std::make_shared<VertexFormat>(
            sizeof(PackedVertex), VertexFormat::standardElements(), true, false);
        auto skinnedVertexFormat = std::make_shared<VertexFormat>(
            static_cast<int>(SKINNED_VERTEX_STRIDE), VertexFormat::skinnedElements(), true, false);

        auto makeDefaultMaterial = []() {
            auto material = std::make_shared<StandardMaterial>();
            material->setName("glTF-default");
            material->setTransparent(false);
            material->setAlphaMode(AlphaMode::OPAQUE);
            material->setMetallicFactor(0.0f);
            material->setRoughnessFactor(1.0f);
            material->setShaderVariantKey(1);
            return material;
        };

        std::vector<std::shared_ptr<Material>> gltfMaterials;
        gltfMaterials.reserve(std::max<size_t>(1, model.materials.size()));
        std::vector<std::shared_ptr<Texture>> gltfTextures(model.textures.size());

        // ── Create GPU textures from pre-converted RGBA data ─────────
        auto getOrCreateTexture = [&](const int textureIndex) -> std::shared_ptr<Texture> {
            if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size())) return nullptr;
            auto& cached = gltfTextures[static_cast<size_t>(textureIndex)];
            if (cached) return cached;

            const auto& srcTexture = model.textures[static_cast<size_t>(textureIndex)];
            int imageSource = srcTexture.source;
            if (imageSource < 0) {
                // Try texture extensions that store the image index in a "source" field
                static const char* textureExtensions[] = {
                    "KHR_texture_basisu",
                    "EXT_texture_webp",
                    "EXT_texture_avif",
                    "MSFT_texture_dds"
                };
                for (const auto* extName : textureExtensions) {
                    auto it = srcTexture.extensions.find(extName);
                    if (it != srcTexture.extensions.end() && it->second.IsObject()) {
                        auto sourceVal = it->second.Get("source");
                        if (sourceVal.IsInt()) {
                            imageSource = sourceVal.GetNumberAsInt();
                            break;
                        }
                    }
                }
            }
            if (imageSource < 0 || imageSource >= static_cast<int>(prepared.images.size())) {
                spdlog::warn("    getOrCreateTexture({}): invalid imageSource={} (extensions: {})",
                    textureIndex, imageSource,
                    [&]() {
                        std::string exts;
                        for (const auto& [k, v] : srcTexture.extensions) {
                            if (!exts.empty()) exts += ", ";
                            exts += k;
                        }
                        return exts.empty() ? "none" : exts;
                    }());
                return nullptr;
            }

            const auto& prepImg = prepared.images[static_cast<size_t>(imageSource)];
            if (!prepImg.valid || (prepImg.rgbaPixels.empty() && !prepImg.isCompressed)) {
                spdlog::warn("    getOrCreateTexture({}): image {} not valid (valid={}, pixels={})",
                    textureIndex, imageSource, prepImg.valid, prepImg.rgbaPixels.size());
                return nullptr;
            }

            const auto& srcImage = model.images[static_cast<size_t>(imageSource)];

            TextureOptions options;
            options.width  = static_cast<uint32_t>(prepImg.width);
            options.height = static_cast<uint32_t>(prepImg.height);
            if (prepImg.isCompressed) {
                // KHR_texture_basisu: pre-transcoded block-compressed levels.
                options.format = static_cast<PixelFormat>(prepImg.compressedFormat);
                options.mipmaps = prepImg.compressedLevels.size() > 1;
                options.numLevels = static_cast<uint32_t>(prepImg.compressedLevels.size());
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
            options.name = srcImage.name.empty() ? srcTexture.name : srcImage.name;

            auto texture = std::make_shared<Texture>(device.get(), options);
            if (prepImg.isCompressed) {
                for (size_t level = 0; level < prepImg.compressedLevels.size(); ++level) {
                    texture->setLevelData(static_cast<uint32_t>(level),
                        prepImg.compressedLevels[level].data(),
                        prepImg.compressedLevels[level].size());
                }
            } else {
                texture->setLevelData(0, prepImg.rgbaPixels.data(), prepImg.rgbaPixels.size());
            }

            if (srcTexture.sampler >= 0 && srcTexture.sampler < static_cast<int>(model.samplers.size())) {
                const auto& sampler = model.samplers[static_cast<size_t>(srcTexture.sampler)];
                if (sampler.minFilter != -1) {
                    auto minFilter = mapMinFilter(sampler.minFilter);
                    if (minFilter == FilterMode::FILTER_NEAREST_MIPMAP_NEAREST ||
                        minFilter == FilterMode::FILTER_LINEAR_MIPMAP_NEAREST ||
                        minFilter == FilterMode::FILTER_NEAREST_MIPMAP_LINEAR ||
                        minFilter == FilterMode::FILTER_LINEAR_MIPMAP_LINEAR) {
                        minFilter = FilterMode::FILTER_LINEAR;
                    }
                    texture->setMinFilter(minFilter);
                }
                if (sampler.magFilter != -1) texture->setMagFilter(mapMagFilter(sampler.magFilter));
                texture->setAddressU(mapWrapMode(sampler.wrapS));
                texture->setAddressV(mapWrapMode(sampler.wrapT));
            }

            texture->upload();
            container->addOwnedTexture(texture);
            cached = texture;
            return cached;
        };

        // ── Create materials ─────────────────────────────────────────
        size_t actualTextureCount = 0;
        if (model.materials.empty()) {
            gltfMaterials.push_back(makeDefaultMaterial());
        } else {
            for (size_t materialIndex = 0; materialIndex < model.materials.size(); ++materialIndex) {
                const auto& srcMaterial = model.materials[materialIndex];
                auto material = std::make_shared<StandardMaterial>();
                material->setName(srcMaterial.name.empty() ? "glTF-material" : srcMaterial.name);

                const auto& pbr = srcMaterial.pbrMetallicRoughness;
                if (pbr.baseColorFactor.size() == 4) {
                    const Color baseColor(
                        static_cast<float>(pbr.baseColorFactor[0]),
                        static_cast<float>(pbr.baseColorFactor[1]),
                        static_cast<float>(pbr.baseColorFactor[2]),
                        static_cast<float>(pbr.baseColorFactor[3]));
                    material->setBaseColorFactor(baseColor);
                    Color diffuseColor(baseColor);
                    diffuseColor.gamma();
                    material->setDiffuse(diffuseColor);
                    material->setOpacity(baseColor.a);
                }
                material->setMetallicFactor(static_cast<float>(pbr.metallicFactor));
                material->setRoughnessFactor(static_cast<float>(pbr.roughnessFactor));
                material->setMetalness(static_cast<float>(pbr.metallicFactor));
                material->setGloss(1.0f - static_cast<float>(pbr.roughnessFactor));

                if (!srcMaterial.alphaMode.empty()) {
                    if (srcMaterial.alphaMode == "BLEND") {
                        material->setAlphaMode(AlphaMode::BLEND);
                        material->setTransparent(true);
                    } else if (srcMaterial.alphaMode == "MASK") {
                        material->setAlphaMode(AlphaMode::MASK);
                    } else {
                        material->setAlphaMode(AlphaMode::OPAQUE);
                    }
                }
                material->setCullMode(srcMaterial.doubleSided ? CullMode::CULLFACE_NONE : CullMode::CULLFACE_BACK);
                material->setAlphaCutoff(static_cast<float>(srcMaterial.alphaCutoff));

                // Diagnostic: log texture indices and extensions for each material
                {
                    std::string exts;
                    for (const auto& [k, v] : srcMaterial.extensions) {
                        if (!exts.empty()) exts += ", ";
                        exts += k;
                    }
                    spdlog::info("  GLB mat[{}] '{}': baseColorTex.idx={}, normalTex.idx={}, mrTex.idx={}, alpha={}, extensions=[{}]",
                        materialIndex, material->name(),
                        pbr.baseColorTexture.index,
                        srcMaterial.normalTexture.index,
                        pbr.metallicRoughnessTexture.index,
                        srcMaterial.alphaMode.empty() ? "OPAQUE" : srcMaterial.alphaMode,
                        exts.empty() ? "none" : exts);
                }

                // Handle KHR_materials_pbrSpecularGlossiness extension
                applySpecularGlossiness(srcMaterial, material.get(), getOrCreateTexture);
                applyVolumeExtensions(srcMaterial, material.get());
                applyClearcoat(srcMaterial, material.get(), getOrCreateTexture);

                if (pbr.baseColorTexture.index >= 0) {
                    if (auto tex = getOrCreateTexture(pbr.baseColorTexture.index)) {
                        material->setBaseColorTexture(tex.get());
                        material->setHasBaseColorTexture(true);
                        material->setBaseColorUvSet(pbr.baseColorTexture.texCoord);
                        spdlog::info("    -> baseColor texture OK ({}x{})", tex->width(), tex->height());
                        actualTextureCount++;
                    } else {
                        spdlog::warn("    -> baseColor texture FAILED for texIdx={}", pbr.baseColorTexture.index);
                    }
                }
                if (srcMaterial.normalTexture.index >= 0) {
                    if (auto tex = getOrCreateTexture(srcMaterial.normalTexture.index)) {
                        material->setNormalTexture(tex.get());
                        material->setHasNormalTexture(true);
                        material->setNormalUvSet(srcMaterial.normalTexture.texCoord);
                        actualTextureCount++;
                    } else {
                        spdlog::warn("    -> normal texture FAILED for texIdx={}", srcMaterial.normalTexture.index);
                    }
                    material->setNormalScale(static_cast<float>(srcMaterial.normalTexture.scale));
                }
                if (pbr.metallicRoughnessTexture.index >= 0) {
                    if (auto tex = getOrCreateTexture(pbr.metallicRoughnessTexture.index)) {
                        material->setMetallicRoughnessTexture(tex.get());
                        material->setHasMetallicRoughnessTexture(true);
                        actualTextureCount++;
                    } else {
                        spdlog::warn("    -> metallicRoughness texture FAILED for texIdx={}", pbr.metallicRoughnessTexture.index);
                    }
                }
                if (srcMaterial.emissiveFactor.size() == 3) {
                    Color emissiveColor(
                        static_cast<float>(srcMaterial.emissiveFactor[0]),
                        static_cast<float>(srcMaterial.emissiveFactor[1]),
                        static_cast<float>(srcMaterial.emissiveFactor[2]), 1.0f);
                    emissiveColor.gamma();
                    material->setEmissiveFactor(emissiveColor);
                    // StandardMaterial::updateUniforms overrides emissiveFactor with
                    // _emissive * _emissiveIntensity — mirror into the authoritative slot.
                    material->setEmissive(emissiveColor);
                }

                uint64_t variant = 1;
                if (material->hasBaseColorTexture()) variant |= (1ull << 1);
                if (material->hasNormalTexture()) variant |= (1ull << 4);
                if (material->hasMetallicRoughnessTexture()) variant |= (1ull << 5);
                if (material->alphaMode() == AlphaMode::BLEND) variant |= (1ull << 2);
                else if (material->alphaMode() == AlphaMode::MASK) variant |= (1ull << 3);
                material->setShaderVariantKey(variant);
                gltfMaterials.push_back(material);
            }
        }

        // ── Create GPU mesh resources from pre-extracted byte data ────
        std::vector<std::vector<size_t>> meshToPayloadIndices(model.meshes.size());
        size_t nextPayloadIndex = 0;

        for (size_t meshIndex = 0; meshIndex < prepared.meshPrimitives.size(); ++meshIndex) {
            for (auto& pd : prepared.meshPrimitives[meshIndex]) {
                if (pd.vertexBytes.empty()) continue;

                VertexBufferOptions vbOptions;
                vbOptions.data = std::move(pd.vertexBytes);
                auto vb = device->createVertexBuffer(
                    pd.skinned ? skinnedVertexFormat : vertexFormat, pd.vertexCount, vbOptions);
                if (!vb) continue;

                std::shared_ptr<IndexBuffer> ib;
                if (pd.indexed && !pd.indexBytes.empty()) {
                    const int indexCount = static_cast<int>(pd.indexBytes.size() / sizeof(uint32_t));
                    ib = device->createIndexBuffer(INDEXFORMAT_UINT32, indexCount, pd.indexBytes);
                }

                auto meshResource = std::make_shared<Mesh>();
                meshResource->setVertexBuffer(vb);
                meshResource->setIndexBuffer(ib, 0);

                Primitive drawPrimitive;
                drawPrimitive.type = mapPrimitiveType(pd.mode);
                drawPrimitive.base = 0;
                drawPrimitive.baseVertex = 0;
                drawPrimitive.count = pd.drawCount;
                drawPrimitive.indexed = pd.indexed;
                meshResource->setPrimitive(drawPrimitive, 0);

                BoundingBox bounds;
                bounds.setCenter((pd.boundsMin + pd.boundsMax) * 0.5f);
                bounds.setHalfExtents((pd.boundsMax - pd.boundsMin) * 0.5f);
                meshResource->setAabb(bounds);

                GlbMeshPayload payload;
                payload.mesh = meshResource;
                payload.material = (pd.materialIndex >= 0 && pd.materialIndex < static_cast<int>(gltfMaterials.size()))
                    ? gltfMaterials[static_cast<size_t>(pd.materialIndex)] : gltfMaterials.front();
                // Morph targets: deltas were extracted on the background thread;
                // build the GPU buffer here on the main thread.
                if (!pd.morphTargets.empty()) {
                    payload.morph = std::make_shared<Morph>(std::move(pd.morphTargets),
                        pd.vertexCount, device.get());
                    payload.morphInitialWeights = std::move(pd.morphInitialWeights);
                }
                container->addMeshPayload(payload);
                meshToPayloadIndices[meshIndex].push_back(nextPayloadIndex++);
            }
        }

        // ── Create node payloads ─────────────────────────────────────
        for (const auto& node : model.nodes) {
            GlbNodePayload nodePayload;
            nodePayload.name = node.name;
            if (!node.matrix.empty()) decomposeNodeMatrix(node.matrix, nodePayload.translation, nodePayload.rotation, nodePayload.scale);
            if (node.translation.size() == 3) nodePayload.translation = Vector3(static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2]));
            if (node.rotation.size() == 4) nodePayload.rotation = Quaternion(static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]), static_cast<float>(node.rotation[3])).normalized();
            if (node.scale.size() == 3) nodePayload.scale = Vector3(static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2]));
            if (node.mesh >= 0 && node.mesh < static_cast<int>(meshToPayloadIndices.size())) {
                const auto& mapped = meshToPayloadIndices[static_cast<size_t>(node.mesh)];
                nodePayload.meshPayloadIndices.insert(nodePayload.meshPayloadIndices.end(), mapped.begin(), mapped.end());
            }
            nodePayload.skinIndex = node.skin;
            nodePayload.children = node.children;
            container->addNodePayload(nodePayload);
        }

        // ── Parse skins (cheap accessor reads — safe on the main thread) ─
        parseSkins(model, container.get());

        // ── Root scene nodes ─────────────────────────────────────────
        int sceneIndex = model.defaultScene;
        if (sceneIndex < 0 && !model.scenes.empty()) sceneIndex = 0;
        if (sceneIndex >= 0 && sceneIndex < static_cast<int>(model.scenes.size())) {
            for (const auto nodeIndex : model.scenes[static_cast<size_t>(sceneIndex)].nodes)
                container->addRootNodeIndex(nodeIndex);
        }

        // ── Attach pre-parsed animation tracks ───────────────────────
        for (auto& [name, track] : prepared.animTracks) {
            container->addAnimTrack(name, track);
        }

        if (prepared.dracoPrimitiveCount > 0) {
            spdlog::info("GLB Draco summary [{}]: primitives={}, decoded={}, failed={}",
                debugName, prepared.dracoPrimitiveCount, prepared.dracoDecodeSuccessCount, prepared.dracoDecodeFailureCount);
        }

        spdlog::info("GLB createFromPrepared [{}]: GPU resources created (texSlots={}, texActual={}, meshes={}, nodes={})",
            debugName, gltfTextures.size(), actualTextureCount, nextPayloadIndex, model.nodes.size());

        return container;
    }
}
