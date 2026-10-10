// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.02.2026
//
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "glbParser.h"
#include "framework/parsers/packedVertex.h"

#include "framework/components/light/lightComponent.h"

#include <algorithm>
#include <atomic>
#include <numbers>
#include <mutex>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
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
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"
#include "stb_image.h"
#include "framework/assets/stbImageFlip.h"

namespace visutwin::canvas
{
    namespace
    {

        struct PackedPointVertex
        {
            float px, py, pz;       // position
            float cr, cg, cb, ca;   // vertex color (RGBA)
        };

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

        // Every other image is kept ENCODED too, marked `as_is` (tinygltf's own flag
        // for "the bytes are the file's, not pixels"), and decoded by prepareFromModel.
        // tinygltf calls this for one image at a time while it parses, so decoding
        // here decodes a model's images one after another on the calling thread, which
        // is most of what loading a textured model costs; prepareFromModel has all of
        // them in hand and decodes them side by side.
        image->width = 0;
        image->height = 0;
        image->component = 0;
        image->bits = 8;
        image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
        image->as_is = true;
        image->image.assign(bytes, bytes + size);
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
        // A glTF node's identity when it has no name: `node_<index>`.
        // The SAME string has to come out of every place
        // that names a node — the entity the container instantiates, the animation
        // target, the skin's bone list — or an unnamed node exists under one name
        // and is animated under another. Unnamed animated nodes are common in
        // exporter output, where only meshes and bones are named, so a channel
        // targeting one must not be skipped. Callers take the name from
        // glbNodeNames, which also makes sibling names unique.
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

        // The name of every node, as the entity, the animation channels and the skin's
        // bone list all spell it: glbNodeName, with the children of one parent made
        // unique in the order the parent lists them — the first keeps its name, the
        // next of the same name gets "1" appended, the one after "2", and so on. A path
        // of names (glbNodePath) cannot tell two siblings called "Wheel" apart, so
        // without this both of their channels would animate the first one. Root nodes
        // keep their names as written.
        std::vector<std::string> glbNodeNames(const tinygltf::Model& model, const std::vector<int>& parents)
        {
            std::vector<std::string> names(model.nodes.size());
            for (size_t i = 0; i < model.nodes.size(); ++i) {
                names[i] = glbNodeName(model, static_cast<int>(i));
            }
            std::unordered_map<std::string, int> seen;
            for (size_t i = 0; i < model.nodes.size(); ++i) {
                seen.clear();
                for (const int child : model.nodes[i].children) {
                    // Each child once, under the parent glbNodeParents gave it.
                    if (child < 0 || child >= static_cast<int>(names.size()) ||
                        parents[static_cast<size_t>(child)] != static_cast<int>(i)) {
                        continue;
                    }
                    std::string& name = names[static_cast<size_t>(child)];
                    if (const auto it = seen.find(name); it != seen.end()) {
                        name += std::to_string(it->second++);
                    } else {
                        seen.emplace(name, 1);
                    }
                }
            }
            return names;
        }

        // The animation target as a PATH of node names from the node's glTF root
        // down to it, joined with '/'. A bare name
        // cannot tell two nodes apart that share it in different branches — a
        // left and a right "Wheel", or a skeleton exported twice — and every such
        // scene would animate only whichever findByName met first. DefaultAnimBinder
        // walks the path and falls back to the leaf name for tracks that were not
        // produced by this parser.
        std::string glbNodePath(const std::vector<std::string>& names, const std::vector<int>& parents, int nodeIndex)
        {
            std::string path = names[static_cast<size_t>(nodeIndex)];
            for (int parent = parents[static_cast<size_t>(nodeIndex)]; parent >= 0;
                 parent = parents[static_cast<size_t>(parent)]) {
                path = names[static_cast<size_t>(parent)] + "/" + path;
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

        // Reads the elements of one accessor, de-quantised through decodeComponent and
        // with the accessor's sparse substitution applied, whatever the component type.
        //
        // Every reader of glTF accessor data goes through this, vertex attributes
        // included: a sparse accessor is legal on any of them, and one with NO
        // bufferView is zeros plus its sparse values (common for morph deltas, and
        // allowed for POSITION too). Reading only the base view drops the overrides,
        // and a base-less POSITION then reads nothing at all.
        //
        // Built once per accessor: the sparse indices are resolved up front into one
        // value pointer per element they substitute, so a read is a lookup, never a
        // scan of the index list. The accessor's TYPE (VEC2/VEC3/...) has to be the
        // one the caller asked for (`expectedType`, -1 for any): a reader that quietly
        // accepted a VEC2 where a VEC3 was wanted would read a neighbouring element's
        // bytes as the third component. `normalized`, when given, replaces the
        // accessor's own flag (skin weights are unit fractions whatever the file says).
        class AccessorReader
        {
        public:
            /// An invalid reader: every read fails.
            AccessorReader() = default;

            AccessorReader(const tinygltf::Model& model, const tinygltf::Accessor& accessor,
                const int expectedType = -1, const std::optional<bool> normalized = std::nullopt)
                : _count(static_cast<size_t>(accessor.count)),
                  _components(tinygltf::GetNumComponentsInType(accessor.type)),
                  _componentType(accessor.componentType),
                  _normalized(normalized.value_or(accessor.normalized))
            {
                if ((expectedType >= 0 && accessor.type != expectedType) || _components <= 0 ||
                    componentBytes(_componentType) <= 0) {
                    return;
                }
                if (accessor.bufferView >= 0) {
                    _base = getAccessorBase(model, accessor);
                    if (!_base) {
                        return;
                    }
                    _stride = static_cast<size_t>(accessorStride(model, accessor));
                }
                if (accessor.sparse.isSparse && accessor.sparse.count > 0 && !resolveSparse(model, accessor)) {
                    return;
                }
                _valid = true;
            }

            [[nodiscard]] bool valid() const { return _valid; }
            [[nodiscard]] size_t count() const { return _count; }
            [[nodiscard]] int components() const { return _components; }

            /// The first `count` components of element `index` into `out`. False for an
            /// invalid reader, an index past the end or more components than the type has.
            bool read(const size_t index, float* out, const int count) const
            {
                if (!_valid || index >= _count || count > _components) {
                    return false;
                }
                const uint8_t* element = _base ? _base + index * _stride : nullptr;
                if (!_sparseValues.empty() && _sparseValues[index]) {
                    element = _sparseValues[index];
                }
                if (!element) {
                    std::fill(out, out + count, 0.0f);
                    return true;
                }
                for (int c = 0; c < count; ++c) {
                    out[c] = decodeComponent(element, _componentType, _normalized, c);
                }
                return true;
            }

        private:
            // The sparse VALUES carry the accessor's own component type, so they are
            // de-quantised exactly like the dense data they override.
            bool resolveSparse(const tinygltf::Model& model, const tinygltf::Accessor& accessor)
            {
                const auto& sparse = accessor.sparse;
                const auto sparseCount = static_cast<size_t>(sparse.count);

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
                const auto valueStride = static_cast<size_t>(_components) *
                    static_cast<size_t>(componentBytes(_componentType));
                const auto* idxPtr = viewBytes(sparse.indices.bufferView, sparse.indices.byteOffset,
                    sparseCount * static_cast<size_t>(idxBytes));
                const auto* valPtr = viewBytes(sparse.values.bufferView, sparse.values.byteOffset,
                    sparseCount * valueStride);
                if (!idxPtr || !valPtr) {
                    return false;
                }

                _sparseValues.assign(_count, nullptr);
                for (size_t i = 0; i < sparseCount; ++i) {
                    size_t index = 0;
                    switch (idxBytes) {
                        case 1: index = idxPtr[i]; break;
                        case 2: index = reinterpret_cast<const uint16_t*>(idxPtr)[i]; break;
                        default: index = reinterpret_cast<const uint32_t*>(idxPtr)[i]; break;
                    }
                    if (index >= _count) {
                        return false;
                    }
                    _sparseValues[index] = valPtr + i * valueStride;
                }
                return true;
            }

            const uint8_t* _base = nullptr;   // null when the accessor has no bufferView
            size_t _stride = 0;
            size_t _count = 0;
            int _components = 0;
            int _componentType = 0;
            bool _normalized = false;
            bool _valid = false;
            // Per element, the sparse value that replaces it, or null; empty when the
            // accessor is not sparse.
            std::vector<const uint8_t*> _sparseValues;
        };

        // Read all data from an accessor into a flat vector of floats.
        // Works for SCALAR, VEC2, VEC3, VEC4, and for every component type glTF
        // allows — the values are de-quantised on the way out: an animation sampler's
        // output may be normalized byte/short in core glTF, and a morph target's deltas
        // may be quantised under KHR_mesh_quantization. Sparse accessors are honoured,
        // including the base-less form morph-target deltas commonly use. An accessor
        // with neither a bufferView nor sparse values is refused rather than read as
        // zeros: an all-zero inverse bind matrix or keyframe list is never what a file
        // that forgot its data means.
        bool readFloatArray(const tinygltf::Model& model, const tinygltf::Accessor& accessor, std::vector<float>& out)
        {
            if (accessor.bufferView < 0 && !accessor.sparse.isSparse) {
                return false;
            }
            const AccessorReader reader(model, accessor);
            if (!reader.valid()) {
                return false;
            }
            const auto components = static_cast<size_t>(reader.components());
            out.assign(reader.count() * components, 0.0f);
            for (size_t i = 0; i < reader.count(); ++i) {
                reader.read(i, out.data() + i * components, reader.components());
            }
            return true;
        }

        // ── GPU skinning + morph target extraction ──────────────────────

        /// Skinned vertex layout: PackedVertex (14 floats) + blendWeights (4) +
        /// blendIndices (4) = 88 bytes. Matches the STRIDE_SKINNED vertex descriptor
        /// (attributes 11/12) and the VT_FEATURE_SKINNING shader path.
        constexpr size_t SKINNED_VERTEX_STRIDE = sizeof(PackedVertex) + 8 * sizeof(float);

        /// Vertex-coloured layout: PackedVertex (14 floats) + an RGBA colour (4) = 72
        /// bytes, the colour at offset 56 — the layout both backends' vertex-colour
        /// paths read (attribute 5). Static meshes only: neither backend has a vertex
        /// stage that reads a colour beside skin weights or morph deltas.
        constexpr size_t COLORED_VERTEX_STRIDE = sizeof(PackedVertex) + 4 * sizeof(float);

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
            // Joints are integers and weights unit fractions whatever the accessor's
            // normalized flag says; the spec allows only these component types.
            const int jointsType = jointsAccessor->componentType;
            const int weightsType = weightsAccessor->componentType;
            if ((jointsType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                 jointsType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) ||
                (weightsType != TINYGLTF_COMPONENT_TYPE_FLOAT &&
                 weightsType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                 weightsType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)) {
                return out;
            }
            const AccessorReader joints(model, *jointsAccessor, TINYGLTF_TYPE_VEC4, false);
            const AccessorReader weights(model, *weightsAccessor, TINYGLTF_TYPE_VEC4, true);
            if (!joints.valid() || !weights.valid()) {
                return out;
            }

            out.joints.resize(vertexCount * 4);
            out.weights.resize(vertexCount * 4);
            for (size_t i = 0; i < vertexCount; ++i) {
                joints.read(i, out.joints.data() + i * 4, 4);
                float w[4] = {0, 0, 0, 0};
                weights.read(i, w, 4);
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
            const std::vector<std::string> nodeNames = glbNodeNames(model, glbNodeParents(model));
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
                            ? nodeNames[static_cast<size_t>(jointNodeIndex)] : std::string());
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
                            // the max, per axis. Without it
                            // a skinned mesh whose targets push it outward could be culled on screen.
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
            AnimTrackList& outTracks)
        {
            if (model.animations.empty()) {
                return;
            }

            const std::vector<int> nodeParents = glbNodeParents(model);
            const std::vector<std::string> nodeNames = glbNodeNames(model, nodeParents);

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

                    // Map glTF target path to the animation property path.
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
                    const std::string nodeName = glbNodePath(nodeNames, nodeParents, channel.target_node);

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

                // Two animations may share a name (nothing in glTF forbids it), and the
                // tracks are keyed by name, so the later one would overwrite the earlier
                // in silence. Keep both, the later under a suffixed name, and say so.
                if (outTracks.contains(trackName)) {
                    std::string unique;
                    for (int n = 1; unique.empty() || outTracks.contains(unique); ++n) {
                        unique = trackName + "_" + std::to_string(n);
                    }
                    spdlog::warn("GLB: two animations are named '{}'; the later one is kept as '{}'",
                        trackName, unique);
                    trackName = unique;
                }
                track->setName(trackName);
                track->setDuration(duration);

                if (!track->curves().empty()) {
                    outTracks.add(trackName, track);
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

        // Decodes a KHR_draco_mesh_compression primitive into vertices and a triangle
        // list. `outHasNormals` / `outHasTangents` say whether the payload carried those
        // attributes (the caller derives what is missing), and `outColors` receives
        // COLOR_0 as RGBA per vertex, or stays empty.
        bool decodeDracoPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive,
            std::vector<PackedVertex>& outVertices, std::vector<uint32_t>& outIndices, Vector3& outMinPos, Vector3& outMaxPos,
            bool& outHasNormals, bool& outHasTangents, std::vector<float>& outColors)
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
            const auto* colorAttr = getDracoAttribute(*dracoMesh, dracoExt, "COLOR_0");
            outHasNormals = normalAttr && normalAttr->num_components() >= 3;
            outHasTangents = tangentAttr && tangentAttr->num_components() >= 4;
            const bool hasColors = colorAttr && colorAttr->num_components() >= 3;

            const int32_t pointCount = dracoMesh->num_points();
            outVertices.resize(static_cast<size_t>(pointCount));
            outColors.clear();
            if (hasColors) {
                outColors.assign(static_cast<size_t>(pointCount) * 4, 1.0f);
            }

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

                // Overwritten by generated face normals when the payload has none.
                std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
                if (outHasNormals) {
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
                if (outHasTangents) {
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

                if (hasColors) {
                    const draco::AttributeValueIndex colorValueIndex = colorAttr->mapped_index(pointIndex);
                    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
                    if (colorValueIndex >= 0) {
                        colorAttr->ConvertValue<float, 4>(colorValueIndex, color.data());
                    }
                    // A three-component colour comes back with a zero fourth component.
                    std::copy_n(color.data(), 3, outColors.data() + static_cast<size_t>(i) * 4);
                }

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

        /// Decodes an image loadImageData left encoded (`as_is`) straight to RGBA8, with
        /// row 0 at the top as every texture here has it. An image stb cannot read
        /// (a .basis payload, say) becomes a 1x1 magenta placeholder so the model's
        /// geometry still loads. Safe on any thread: the flip is per thread.
        void decodeEncodedImage(const tinygltf::Image& image, const size_t imageIndex,
            PreparedGlbData::ImageData& out)
        {
            int width = 0;
            int height = 0;
            int components = 0;
            stbi_uc* decoded = nullptr;
            {
                // Per-thread flip state, restored after the decode so it cannot leak
                // into the next image loaded on this thread (see stbImageFlip.h).
                const StbVerticalFlipScope flipScope(true);
                decoded = stbi_load_from_memory(image.image.data(), static_cast<int>(image.image.size()),
                    &width, &height, &components, 4);
            }
            if (!decoded) {
                const char* reason = stbi_failure_reason();
                spdlog::warn("GLB image #{}: stb_image cannot decode ({}), mimeType={} — using placeholder",
                    imageIndex, reason ? reason : "unknown", image.mimeType);
                out.rgbaPixels = {255, 0, 255, 255};
                out.width = 1;
                out.height = 1;
                out.valid = true;
                return;
            }
            const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            out.rgbaPixels.assign(decoded, decoded + byteCount);
            stbi_image_free(decoded);
            out.width = width;
            out.height = height;
            out.valid = true;
        }

        /// Runs body(i) for every i in [0, count), on this thread and as many others as
        /// there is work and hardware for. Returns when all of it is done. The bodies
        /// must not touch shared state: here each one fills its own image.
        template <typename Body>
        void parallelFor(const size_t count, Body&& body)
        {
            const size_t workers = std::min<size_t>(count,
                std::max<size_t>(1, std::thread::hardware_concurrency()));
            if (workers <= 1) {
                for (size_t i = 0; i < count; ++i) {
                    body(i);
                }
                return;
            }
            std::atomic<size_t> next{0};
            const auto run = [&] {
                for (size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
                    body(i);
                }
            };
            std::vector<std::thread> threads;
            threads.reserve(workers - 1);
            for (size_t t = 1; t < workers; ++t) {
                threads.emplace_back(run);
            }
            run();
            for (auto& thread : threads) {
                thread.join();
            }
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

            // Match Quaternion::fromMatrix4's convention for mirrored transforms:
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
     * A material carrying transmission OR volume
     * is made blended and switched to dynamic (grab-pass) refraction: without both it
     * renders in the opaque pass and refracts only the environment. Blending
     * is setTransparent(true), NOT setAlphaMode(BLEND), which would also
     * turn depth writes off.
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
    // EXT_mesh_gpu_instancing: the node's TRANSLATION,
    // ROTATION and SCALE accessors become one column-major TRS matrix per instance, in
    // the node's local space, packed as the 64-byte default instancing format. The
    // count is the first attribute's; an attribute that is absent is identity. Every
    // component type the spec allows (normalized rotations included) goes through
    // AccessorReader's de-quantisation. Empty when the node has no usable extension.
    static std::vector<float> gltfInstanceMatrices(const tinygltf::Model& model, const tinygltf::Node& node)
    {
        const auto ext = node.extensions.find("EXT_mesh_gpu_instancing");
        if (ext == node.extensions.end() || !ext->second.IsObject() || !ext->second.Has("attributes")) {
            return {};
        }
        const auto& attributes = ext->second.Get("attributes");
        const auto accessorFor = [&](const char* name, const int type) -> const tinygltf::Accessor* {
            if (!attributes.Has(name) || !attributes.Get(name).IsInt()) {
                return nullptr;
            }
            const int index = attributes.Get(name).GetNumberAsInt();
            if (index < 0 || index >= static_cast<int>(model.accessors.size()) ||
                model.accessors[static_cast<size_t>(index)].type != type) {
                return nullptr;
            }
            return &model.accessors[static_cast<size_t>(index)];
        };
        const auto* translations = accessorFor("TRANSLATION", TINYGLTF_TYPE_VEC3);
        const auto* rotations = accessorFor("ROTATION", TINYGLTF_TYPE_VEC4);
        const auto* scales = accessorFor("SCALE", TINYGLTF_TYPE_VEC3);
        const size_t count = translations ? translations->count : rotations ? rotations->count : scales ? scales->count : 0;
        if (count == 0) {
            return {};
        }

        const AccessorReader translationReader = translations
            ? AccessorReader(model, *translations, TINYGLTF_TYPE_VEC3) : AccessorReader();
        const AccessorReader rotationReader = rotations
            ? AccessorReader(model, *rotations, TINYGLTF_TYPE_VEC4) : AccessorReader();
        const AccessorReader scaleReader = scales
            ? AccessorReader(model, *scales, TINYGLTF_TYPE_VEC3) : AccessorReader();
        std::vector<float> matrices(count * 16);
        for (size_t i = 0; i < count; ++i) {
            // A read that fails (an absent attribute, or one shorter than the first)
            // leaves the identity component in place.
            float t[3] = {0.0f, 0.0f, 0.0f};
            float r[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            float sc[3] = {1.0f, 1.0f, 1.0f};
            translationReader.read(i, t, 3);
            rotationReader.read(i, r, 4);
            scaleReader.read(i, sc, 3);
            const Matrix4 matrix = Matrix4::trs(Vector3(t[0], t[1], t[2]),
                Quaternion(r[0], r[1], r[2], r[3]).normalized(), Vector3(sc[0], sc[1], sc[2]));
            matrix.store(matrices.data() + i * 16);
        }
        return matrices;
    }

    // A glTF camera: perspective yfov in radians
    // to degrees, orthographic ymag as the half height, a manual aspect only where
    // the file gives one (perspective aspectRatio, orthographic xmag / ymag), and the
    // far plane only when present — glTF's infinite perspective has none.
    static GlbCameraPayload gltfCameraPayload(const tinygltf::Camera& camera)
    {
        GlbCameraPayload payload;
        if (camera.type == "orthographic") {
            const auto& ortho = camera.orthographic;
            payload.projection = ProjectionType::Orthographic;
            payload.nearClip = static_cast<float>(ortho.znear);
            if (ortho.zfar > 0.0) {
                payload.farClip = static_cast<float>(ortho.zfar);
            }
            payload.orthoHeight = static_cast<float>(ortho.ymag);
            if (ortho.xmag != 0.0 && ortho.ymag != 0.0) {
                payload.aspectRatio = static_cast<float>(ortho.xmag / ortho.ymag);
            }
        } else {
            const auto& perspective = camera.perspective;
            payload.projection = ProjectionType::Perspective;
            payload.nearClip = static_cast<float>(perspective.znear);
            if (perspective.zfar > 0.0) {
                payload.farClip = static_cast<float>(perspective.zfar);
            }
            payload.fovDegrees = static_cast<float>(perspective.yfov * 180.0 / std::numbers::pi);
            if (perspective.aspectRatio > 0.0) {
                payload.aspectRatio = static_cast<float>(perspective.aspectRatio);
            }
        }
        return payload;
    }

    // A KHR_lights_punctual light. "point" is an
    // omni light, cone angles go from radians to degrees (defaults 0 and 45), an
    // absent range becomes 9999 (infinity would poison the bounds), the falloff is
    // inverse-squared. The file's intensity is photometric (candela, lux), so it is
    // stored twice: as the LUMINANCE, times the unit conversion, which a scene with
    // physical units shines with (luminance / conversion gives the file's value
    // back), and CLAMPED to [0, 2] as the intensity everything else uses. The colour
    // is taken as the file gives it.
    // DEVIATION: an intensity the file leaves out is its spec default of 1, where
    // upstream leaves the luminance at 0 — tinygltf does not say which it was.
    static GlbLightPayload gltfLightPayload(const tinygltf::Light& light)
    {
        GlbLightPayload payload;
        if (light.type == "directional") {
            payload.type = LightType::LIGHTTYPE_DIRECTIONAL;
        } else if (light.type == "spot") {
            payload.type = LightType::LIGHTTYPE_SPOT;
            payload.innerConeDegrees = static_cast<float>(light.spot.innerConeAngle * 180.0 / std::numbers::pi);
            payload.outerConeDegrees = static_cast<float>(light.spot.outerConeAngle * 180.0 / std::numbers::pi);
        } else {
            payload.type = LightType::LIGHTTYPE_OMNI;
        }
        if (light.color.size() >= 3) {
            payload.color = Color(static_cast<float>(light.color[0]), static_cast<float>(light.color[1]),
                static_cast<float>(light.color[2]), 1.0f);
        }
        payload.intensity = std::clamp(static_cast<float>(light.intensity), 0.0f, 2.0f);
        constexpr double degToRad = std::numbers::pi / 180.0;
        payload.luminance = static_cast<float>(light.intensity) * LightComponent::lightUnitConversion(payload.type,
            static_cast<float>(payload.outerConeDegrees * degToRad), static_cast<float>(payload.innerConeDegrees * degToRad));
        if (light.range > 0.0) {
            payload.range = static_cast<float>(light.range);
        }
        return payload;
    }

    static void warnUnsupportedRequiredExtensions(const tinygltf::Model& model, const std::string& debugName)
    {
        // What the parser actually acts on. The texture-container extensions are
        // here because the parser resolves the image index through them; whether
        // the image PAYLOAD decodes is a separate question the image loader
        // answers with its own per-image warning.
        static const std::set<std::string> supported = {
            "EXT_mesh_gpu_instancing",
            "KHR_draco_mesh_compression",
            "KHR_gaussian_splatting",
            "KHR_lights_punctual",
            "KHR_materials_anisotropy",
            "KHR_materials_clearcoat",
            "KHR_materials_dispersion",
            "KHR_materials_emissive_strength",
            "KHR_materials_ior",
            "KHR_materials_iridescence",
            "KHR_materials_pbrSpecularGlossiness",
            "KHR_materials_sheen",
            "KHR_materials_specular",
            "KHR_materials_transmission",
            "KHR_materials_unlit",
            "KHR_materials_variants",
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
     * DEVIATION from upstream's texture-transform code, which cannot be copied
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
     * DEVIATION: the extension's own `texCoord` override is ignored. The texture info's texCoord still selects the UV set.
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
     * setClearCoatGloss + setClearCoatGlossInvert(true). The intensity map is
     * read from R and the roughness map from G, as the extension stores them.
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
            if (const auto tex = getOrCreateTexture(idx)) {
                material->setClearCoatMap(tex.get());
                material->setClearCoatMapChannel(MapChannel::MAP_CHANNEL_R);
            }
        }
        if (const int idx = textureIndex("clearcoatRoughnessTexture"); idx >= 0) {
            if (const auto tex = getOrCreateTexture(idx)) {
                material->setClearCoatGlossMap(tex.get());
                material->setClearCoatGlossMapChannel(MapChannel::MAP_CHANNEL_G);
            }
        }
        if (const int idx = textureIndex("clearcoatNormalTexture"); idx >= 0) {
            if (const auto tex = getOrCreateTexture(idx)) material->setClearCoatNormalMap(tex.get());
            // The texture info's scale scales the coat normal map's XY, as the base
            // normalTexture.scale does the base normal map (spec default 1).
            const auto info = cc.Get("clearcoatNormalTexture");
            material->setClearCoatBumpiness(
                info.Has("scale") && info.Get("scale").IsNumber()
                    ? static_cast<float>(info.Get("scale").GetNumberAsDouble()) : 1.0f);
        }
    }

    // Readers for a material extension object. A missing or mistyped key answers
    // the fallback, which is the extension's spec default at every call site.
    static float extensionNumber(const tinygltf::Value& ext, const char* key, const float fallback)
    {
        if (ext.Has(key)) {
            if (const auto& v = ext.Get(key); v.IsNumber()) {
                return static_cast<float>(v.GetNumberAsDouble());
            }
        }
        return fallback;
    }

    static Color extensionColor(const tinygltf::Value& ext, const char* key, const Color& fallback)
    {
        if (ext.Has(key)) {
            const auto& v = ext.Get(key);
            if (v.IsArray() && v.ArrayLen() >= 3 && v.Get(0).IsNumber() && v.Get(1).IsNumber() && v.Get(2).IsNumber()) {
                return Color(static_cast<float>(v.Get(0).GetNumberAsDouble()),
                    static_cast<float>(v.Get(1).GetNumberAsDouble()),
                    static_cast<float>(v.Get(2).GetNumberAsDouble()), 1.0f);
            }
        }
        return fallback;
    }

    // DEVIATION: the sheen, specular, iridescence and anisotropy extensions all allow
    // TEXTURES, and this engine's material has none of those maps (the fragment stage
    // is at MoltenVK's sampler limit, and no shader reads a sheen or iridescence
    // map). The factors are applied; a texture is ignored with
    // one warning per extension and process, so the file still loads.
    static void warnIgnoredExtensionTextures(const tinygltf::Value& ext, const char* extension,
        std::initializer_list<const char*> textureKeys)
    {
        static std::mutex mutex;
        static std::set<std::string> warned;
        for (const char* key : textureKeys) {
            if (!ext.Has(key)) {
                continue;
            }
            const std::lock_guard lock(mutex);
            if (warned.insert(extension).second) {
                spdlog::warn("GLB: {} textures are not supported; '{}' and its siblings are ignored, "
                    "the extension's factors still apply", extension, key);
            }
            return;
        }
    }

    static const tinygltf::Value* materialExtension(const tinygltf::Material& srcMaterial, const char* name)
    {
        const auto it = srcMaterial.extensions.find(name);
        return it != srcMaterial.extensions.end() && it->second.IsObject() ? &it->second : nullptr;
    }

    /**
     * KHR_materials_sheen: the colour factor is
     * linear in the file and stored gamma-encoded (both shaders decode it), the
     * roughness factor is the sheen roughness. DEVIATION: an absent colour factor is
     * the spec's default of BLACK — no sheen — where upstream substitutes white.
     */
    static void applySheen(const tinygltf::Material& srcMaterial, StandardMaterial* material)
    {
        const auto* ext = materialExtension(srcMaterial, "KHR_materials_sheen");
        if (!ext) {
            return;
        }
        Color color = extensionColor(*ext, "sheenColorFactor", Color(0.0f, 0.0f, 0.0f, 1.0f));
        color.gamma();
        material->setSheenColor(color);
        material->setSheenRoughness(extensionNumber(*ext, "sheenRoughnessFactor", 0.0f));
        warnIgnoredExtensionTextures(*ext, "KHR_materials_sheen", {"sheenColorTexture", "sheenRoughnessTexture"});
    }

    /**
     * KHR_materials_specular, for a metallic-rough
     * material: the colour factor tints the non-metal F0 (stored gamma-encoded),
     * the factor scales it; metals are untouched.
     */
    static void applySpecularExtension(const tinygltf::Material& srcMaterial, StandardMaterial* material)
    {
        const auto* ext = materialExtension(srcMaterial, "KHR_materials_specular");
        if (!ext || !material->useMetalness()) {
            return;
        }
        Color color = extensionColor(*ext, "specularColorFactor", Color(1.0f, 1.0f, 1.0f, 1.0f));
        color.gamma();
        material->setSpecular(color);
        material->setUseMetalnessSpecularColor(true);
        material->setSpecularityFactor(extensionNumber(*ext, "specularFactor", 1.0f));
        warnIgnoredExtensionTextures(*ext, "KHR_materials_specular", {"specularTexture", "specularColorTexture"});
    }

    /**
     * KHR_materials_iridescence: the factor, the thin film's IOR (stored as an IOR
     * here, see StandardMaterial) and its thickness. Without a thickness texture the
     * spec's film is the MAXIMUM thickness; the minimum only matters to the texture.
     */
    static void applyIridescence(const tinygltf::Material& srcMaterial, StandardMaterial* material)
    {
        const auto* ext = materialExtension(srcMaterial, "KHR_materials_iridescence");
        if (!ext) {
            return;
        }
        material->setIridescenceIntensity(extensionNumber(*ext, "iridescenceFactor", 0.0f));
        material->setIridescenceIOR(extensionNumber(*ext, "iridescenceIor", 1.3f));
        material->setIridescenceThicknessMax(extensionNumber(*ext, "iridescenceThicknessMaximum", 400.0f));
        warnIgnoredExtensionTextures(*ext, "KHR_materials_iridescence", {"iridescenceTexture", "iridescenceThicknessTexture"});
    }

    /**
     * KHR_materials_anisotropy: the strength and the rotation (radians in the file,
     * degrees on the material, from the tangent toward the bitangent).
     */
    static void applyAnisotropyExtension(const tinygltf::Material& srcMaterial, StandardMaterial* material)
    {
        const auto* ext = materialExtension(srcMaterial, "KHR_materials_anisotropy");
        if (!ext) {
            return;
        }
        // Anisotropy is a GGX lobe, so the extension turns GGX direct specular on.
        material->setEnableGGXSpecular(true);
        material->setAnisotropy(extensionNumber(*ext, "anisotropyStrength", 0.0f));
        material->setAnisotropyRotation(static_cast<float>(
            extensionNumber(*ext, "anisotropyRotation", 0.0f) * 180.0 / std::numbers::pi));
        warnIgnoredExtensionTextures(*ext, "KHR_materials_anisotropy", {"anisotropyTexture"});
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

        // diffuseFactor → the diffuse colour and the opacity (its 4th component), white
        // and opaque when absent. Opacity only: whether the material blends is the
        // file's alphaMode, which createGltfMaterial has already applied. An alpha
        // below 1 does not turn an OPAQUE material into a blended one, and a BLEND
        // material keeps exactly the alpha the file gives it.
        Color diffColor(1.0f, 1.0f, 1.0f, 1.0f);
        if (sg.Has("diffuseFactor")) {
            const auto df = sg.Get("diffuseFactor");
            if (df.IsArray() && df.ArrayLen() >= 3) {
                const auto component = [&df](const int i) {
                    return df.Get(i).IsNumber() ? static_cast<float>(df.Get(i).GetNumberAsDouble()) : 1.0f;
                };
                diffColor = Color(component(0), component(1), component(2), df.ArrayLen() >= 4 ? component(3) : 1.0f);
            }
        }
        material->setBaseColorFactor(diffColor);
        Color gammaColor(diffColor);
        gammaColor.gamma();
        material->setDiffuse(gammaColor);
        material->setOpacity(diffColor.a);

        // diffuseTexture → baseColorTexture
        if (sg.Has("diffuseTexture")) {
            const auto dt = sg.Get("diffuseTexture");
            if (dt.IsObject() && dt.Has("index")) {
                const int texIdx = dt.Get("index").GetNumberAsInt();
                if (auto tex = getOrCreateTexture(texIdx)) {
                    // Set on BOTH base Material and StandardMaterial paths
                    material->setBaseColorTexture(tex.get());
                    material->setHasBaseColorTexture(true);
                    material->setDiffuseMap(tex.get());
                    if (dt.Has("texCoord"))
                        material->setBaseColorUvSet(dt.Get("texCoord").GetNumberAsInt());
                } else {
                    spdlog::warn("GLB material '{}': spec-gloss diffuse texture {} could not be created",
                        material->name(), texIdx);
                }
            }
        }

        // The specular workflow of the KHR_materials_pbrSpecularGlossiness
        // extension: useMetalness off, `specular` stored in sRGB (the factor
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
    // and createFromPrepared (the two asynchronous ones). Never give a path its own
    // copy: copies drift, and nothing notices because no example loads asynchronously.
    // `textureCount`, when given, counts the core textures bound.
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
        // glTF metallic-roughness: useMetalness is set for every glTF material.
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
        applySheen(srcMaterial, material.get());
        applySpecularExtension(srcMaterial, material.get());
        applyIridescence(srcMaterial, material.get());
        applyAnisotropyExtension(srcMaterial, material.get());
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
    // worker. Texture creation, vertex extraction and node building live here once:
    // per-path copies drift, and no example loads asynchronously to notice.

    namespace
    {
        const tinygltf::Accessor* primitiveAttribute(const tinygltf::Model& model,
            const tinygltf::Primitive& primitive, const char* name)
        {
            const auto it = primitive.attributes.find(name);
            return it != primitive.attributes.end() ? getAccessor(model, it->second) : nullptr;
        }

        bool isGaussianSplatPrimitive(const tinygltf::Primitive& primitive)
        {
            return primitive.extensions.contains("KHR_gaussian_splatting");
        }

        // KHR_gaussian_splatting: the splat attributes of a
        // POINTS primitive, read through AccessorReader so every component type the file
        // may use is de-quantised. The values are ACTIVATED — linear scale, post-sigmoid
        // opacity. Null, with an error, when a required attribute is missing or its
        // count does not match POSITION's. SH bands count only while complete.
        // The extension's sortingMethod and projection are
        // ignored; an unsupported kernel or a linear colour
        // space is warned about and rendered as the default.
        std::unique_ptr<GSplatData> gaussianSplatData(const tinygltf::Model& model,
            const tinygltf::Primitive& primitive, const std::string& source)
        {
            static constexpr const char* kExt = "KHR_gaussian_splatting";
            const auto& ext = primitive.extensions.at(kExt);
            if (ext.IsObject() && ext.Has("kernel") && ext.Get("kernel").IsString() &&
                ext.Get("kernel").Get<std::string>() != "ellipse") {
                spdlog::warn("GLB [{}]: {} kernel '{}' is not supported, rendering as 'ellipse'", source, kExt,
                    ext.Get("kernel").Get<std::string>());
            }
            if (ext.IsObject() && ext.Has("colorSpace") && ext.Get("colorSpace").IsString() &&
                ext.Get("colorSpace").Get<std::string>() == "lin_rec709_display") {
                spdlog::warn("GLB [{}]: {} colour space 'lin_rec709_display' is not supported, "
                    "treated as srgb_rec709_display", source, kExt);
            }

            const auto* positions = primitiveAttribute(model, primitive, "POSITION");
            const size_t count = positions ? positions->count : 0;
            const auto read = [&](const std::string& name, const int type, const int components,
                                  std::vector<float>& out) -> bool {
                const auto* accessor = primitiveAttribute(model, primitive, name.c_str());
                if (!accessor || accessor->count != count || accessor->type != type) {
                    return false;
                }
                const AccessorReader reader(model, *accessor, type);
                out.resize(count * static_cast<size_t>(components));
                for (size_t i = 0; i < count; ++i) {
                    if (!reader.read(i, out.data() + i * static_cast<size_t>(components), components)) {
                        return false;
                    }
                }
                return true;
            };

            GSplatData::ActivatedSplats splats;
            splats.count = count;
            const std::string prefix = std::string(kExt) + ":";
            if (count == 0 || !read("POSITION", TINYGLTF_TYPE_VEC3, 3, splats.positions) ||
                !read(prefix + "ROTATION", TINYGLTF_TYPE_VEC4, 4, splats.rotations) ||
                !read(prefix + "SCALE", TINYGLTF_TYPE_VEC3, 3, splats.scales) ||
                !read(prefix + "OPACITY", TINYGLTF_TYPE_SCALAR, 1, splats.opacities) ||
                !read(prefix + "SH_DEGREE_0_COEF_0", TINYGLTF_TYPE_VEC3, 3, splats.sh0)) {
                spdlog::error("GLB [{}]: a {} primitive is missing required attributes or their data is invalid; "
                    "the primitive is skipped", source, kExt);
                return nullptr;
            }

            // Higher bands: degree d has 2d + 1 coefficients, each an RGB attribute.
            // Gathered coefficient-major, the layout the splat data keeps.
            static constexpr int kDegreeCoeffs[] = {3, 5, 7};
            int bands = 0;
            for (int d = 1; d <= 3; ++d) {
                bool complete = true;
                for (int c = 0; c < kDegreeCoeffs[d - 1]; ++c) {
                    if (!primitive.attributes.contains(prefix + "SH_DEGREE_" + std::to_string(d) + "_COEF_" + std::to_string(c))) {
                        complete = false;
                        break;
                    }
                }
                if (!complete) {
                    break;
                }
                bands = d;
            }
            if (bands > 0) {
                static constexpr int kBandCoeffs[] = {0, 3, 8, 15};
                const int coeffs = kBandCoeffs[bands];
                splats.shRest.assign(count * static_cast<size_t>(coeffs) * 3, 0.0f);
                int k = 0;
                std::vector<float> coefficient;
                for (int d = 1; d <= bands; ++d) {
                    for (int c = 0; c < kDegreeCoeffs[d - 1]; ++c, ++k) {
                        const std::string name = prefix + "SH_DEGREE_" + std::to_string(d) + "_COEF_" + std::to_string(c);
                        if (!read(name, TINYGLTF_TYPE_VEC3, 3, coefficient)) {
                            spdlog::error("GLB [{}]: a {} primitive has an invalid {} attribute; the primitive is skipped",
                                source, kExt, name);
                            return nullptr;
                        }
                        for (size_t i = 0; i < count; ++i) {
                            float* dst = splats.shRest.data() + (i * static_cast<size_t>(coeffs) + static_cast<size_t>(k)) * 3;
                            dst[0] = coefficient[i * 3];
                            dst[1] = coefficient[i * 3 + 1];
                            dst[2] = coefficient[i * 3 + 2];
                        }
                    }
                }
                splats.shBands = bands;
            }
            return GSplatData::fromActivated(splats, source);
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
                    // sixteen values are the four columns in sequence (setElement takes
                    // (col, row), and swapping them writes the matrix TRANSPOSED);
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
            const AccessorReader positionReader(model, positions, TINYGLTF_TYPE_VEC3);
            // COLOR_0 is VEC3 or VEC4; a VEC3 colour keeps alpha 1.
            const AccessorReader colorReader = colors &&
                (colors->type == TINYGLTF_TYPE_VEC3 || colors->type == TINYGLTF_TYPE_VEC4)
                ? AccessorReader(model, *colors) : AccessorReader();
            const int colorComponents = colorReader.valid() ? colorReader.components() : 0;
            for (size_t i = 0; i < count; ++i) {
                float p[3] = {0.0f, 0.0f, 0.0f};
                if (!positionReader.read(i, p, 3)) {
                    continue;
                }
                Vector3 pos = Vector3::load(p);
                if (transform) {
                    pos = transform->transformPoint(pos);
                }
                float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                if (colorComponents > 0) {
                    colorReader.read(i, color, colorComponents);
                }
                out[base + i] = PackedPointVertex{pos.getX(), pos.getY(), pos.getZ(),
                    color[0], color[1], color[2], color[3]};
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

        // KHR_materials_variants on a primitive: each
        // mapping gives one material to a list of variant indices.
        std::vector<std::pair<int, int>> primitiveVariantMaterials(const tinygltf::Primitive& primitive)
        {
            std::vector<std::pair<int, int>> result;
            const auto ext = primitive.extensions.find("KHR_materials_variants");
            if (ext == primitive.extensions.end() || !ext->second.IsObject() || !ext->second.Has("mappings")) {
                return result;
            }
            const auto& mappings = ext->second.Get("mappings");
            for (size_t m = 0; mappings.IsArray() && m < mappings.ArrayLen(); ++m) {
                const auto& mapping = mappings.Get(static_cast<int>(m));
                if (!mapping.IsObject() || !mapping.Has("material") || !mapping.Has("variants")) {
                    continue;
                }
                const int material = mapping.Get("material").GetNumberAsInt();
                const auto& variants = mapping.Get("variants");
                for (size_t v = 0; variants.IsArray() && v < variants.ArrayLen(); ++v) {
                    result.emplace_back(variants.Get(static_cast<int>(v)).GetNumberAsInt(), material);
                }
            }
            return result;
        }

        // The glTF triangle modes: a list, a strip and a fan.
        bool isTriangleMode(const int mode)
        {
            return mode == TINYGLTF_MODE_TRIANGLES || mode == TINYGLTF_MODE_TRIANGLE_STRIP ||
                mode == TINYGLTF_MODE_TRIANGLE_FAN;
        }

        // The triangles of a triangle-mode primitive as corner vertex indices, three per
        // triangle, in the order the glTF specification defines for each mode: a strip
        // swaps the last two corners of every odd triangle, so all of them face the way
        // the first one does, and a fan turns around its first vertex. `indices` empty
        // means the vertices in order. A triangle naming a vertex past the end is dropped.
        std::vector<uint32_t> triangleCorners(const int mode, const std::vector<uint32_t>& indices,
            const size_t vertexCount)
        {
            const size_t n = indices.empty() ? vertexCount : indices.size();
            const auto at = [&](const size_t i) {
                return indices.empty() ? static_cast<uint32_t>(i) : indices[i];
            };
            std::vector<uint32_t> corners;
            const auto add = [&](const uint32_t a, const uint32_t b, const uint32_t c) {
                if (a < vertexCount && b < vertexCount && c < vertexCount) {
                    corners.insert(corners.end(), {a, b, c});
                }
            };
            if (mode == TINYGLTF_MODE_TRIANGLES) {
                corners.reserve(n - n % 3);
                for (size_t i = 0; i + 2 < n; i += 3) {
                    add(at(i), at(i + 1), at(i + 2));
                }
            } else if (mode == TINYGLTF_MODE_TRIANGLE_STRIP) {
                for (size_t i = 0; i + 2 < n; ++i) {
                    const bool odd = (i & 1) != 0;
                    add(at(i), at(i + (odd ? 2 : 1)), at(i + (odd ? 1 : 2)));
                }
            } else if (mode == TINYGLTF_MODE_TRIANGLE_FAN) {
                for (size_t i = 1; i + 1 < n; ++i) {
                    add(at(i), at(i + 1), at(0));
                }
            }
            return corners;
        }

        // A triangle primitive without NORMAL is FLAT shaded, as the glTF specification
        // requires: every triangle gets three vertices of its own carrying its face
        // normal, cross(p1 - p0, p2 - p0) normalised (a zero-area triangle takes +Y).
        // On return the primitive is a non-indexed triangle list. Returns the source
        // vertex each resulting vertex was copied from, so the streams read beside the
        // vertices (skin influences, morph deltas, colours) can be re-indexed the same
        // way; empty when the vertices already were one triple per triangle and only
        // their normals changed.
        //
        // DEVIATION: upstream keeps smoothly averaged normals in the vertex buffer and
        // flat shades through a material flag (normals from screen-space derivatives),
        // which this engine's materials do not have. Unwelding gives the same facets
        // without a shader change, at the cost of three vertices per triangle, and the
        // flat look cannot be switched back to smooth normals afterwards.
        std::vector<uint32_t> applyFlatNormals(std::vector<PackedVertex>& vertices, std::vector<uint32_t>& indices,
            int& mode)
        {
            std::vector<uint32_t> corners = triangleCorners(mode, indices, vertices.size());
            const bool inPlace = mode == TINYGLTF_MODE_TRIANGLES && indices.empty() &&
                corners.size() == vertices.size();
            if (inPlace) {
                corners.clear();
            } else {
                std::vector<PackedVertex> unwelded;
                unwelded.reserve(corners.size());
                for (const uint32_t source : corners) {
                    unwelded.push_back(vertices[source]);
                }
                vertices = std::move(unwelded);
            }
            for (size_t t = 0; t + 2 < vertices.size(); t += 3) {
                const Vector3 p0 = Vector3::load(&vertices[t].px);
                const Vector3 faceNormal = (Vector3::load(&vertices[t + 1].px) - p0).cross(
                    Vector3::load(&vertices[t + 2].px) - p0);
                const float length = faceNormal.length();
                const Vector3 normal = length > 0.0f ? faceNormal * (1.0f / length) : Vector3(0.0f, 1.0f, 0.0f);
                for (size_t k = 0; k < 3; ++k) {
                    vertices[t + k].nx = normal.getX();
                    vertices[t + k].ny = normal.getY();
                    vertices[t + k].nz = normal.getZ();
                }
            }
            indices.clear();
            mode = TINYGLTF_MODE_TRIANGLES;
            return corners;
        }

        // Re-indexes a per-vertex stream of `width` values per vertex after
        // applyFlatNormals (`source` empty: nothing to do). A stream shorter than the
        // `sourceCount` vertices it should cover cannot be re-indexed and is cleared.
        void remapPerVertex(std::vector<float>& values, const std::vector<uint32_t>& source,
            const size_t sourceCount, const size_t width)
        {
            if (source.empty() || values.empty()) {
                return;
            }
            if (values.size() < sourceCount * width) {
                values.clear();
                return;
            }
            std::vector<float> remapped(source.size() * width);
            for (size_t i = 0; i < source.size(); ++i) {
                std::copy_n(values.data() + static_cast<size_t>(source[i]) * width, width, remapped.data() + i * width);
            }
            values = std::move(remapped);
        }

        // Interleaves the vertices with COLOR_0 (RGBA per vertex) into the 72-byte
        // vertex-coloured layout. glTF vertex colours are LINEAR, and so is what the
        // vertex stage passes on unless a material sets vertexColorGamma, which a
        // glTF material never does: the file's values are stored as they are. Alpha
        // is written as 1: the colour tints the diffuse only, and opacity is not taken
        // from it.
        std::vector<uint8_t> packColoredVertices(const std::vector<PackedVertex>& vertices,
            const std::vector<float>& colors)
        {
            std::vector<uint8_t> bytes(vertices.size() * COLORED_VERTEX_STRIDE);
            for (size_t i = 0; i < vertices.size(); ++i) {
                auto* dst = bytes.data() + i * COLORED_VERTEX_STRIDE;
                std::memcpy(dst, &vertices[i], sizeof(PackedVertex));
                float color[4];
                for (size_t c = 0; c < 3; ++c) {
                    color[c] = std::max(colors[i * 4 + c], 0.0f);
                }
                color[3] = 1.0f;
                std::memcpy(dst + sizeof(PackedVertex), color, sizeof(color));
            }
            return bytes;
        }

        // Vertices, indices, skin attributes and morph targets of one non-POINTS
        // primitive. False when the primitive has nothing drawable.
        //
        // Every attribute is read through AccessorReader (sparse and quantised data
        // included). A triangle primitive without NORMAL is flat shaded
        // (applyFlatNormals), and the tangents a primitive without TANGENT gets are
        // derived afterwards, from the final normals. COLOR_0 takes the 72-byte
        // vertex-coloured layout on a static mesh; on a skinned or morphed one it is
        // dropped with a warning, and a colour that is white everywhere is dropped
        // silently, since it multiplies by one and would only cost the mesh its
        // batchable layout.
        bool extractTrianglePrimitive(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
            const tinygltf::Primitive& primitive, const size_t meshIndex, PreparedGlbData& counters,
            PreparedGlbData::PrimitiveData& pd)
        {
            // An absent mode is TRIANGLES (the specification's default).
            int mode = primitive.mode < 0 ? TINYGLTF_MODE_TRIANGLES : primitive.mode;
            pd.materialIndex = primitive.material;
            pd.variantMaterials = primitiveVariantMaterials(primitive);

            std::vector<PackedVertex> vertices;
            std::vector<uint32_t> parsedIndices;
            std::vector<float> colors;   // RGBA per vertex, empty without COLOR_0
            bool hasNormals = false;
            bool hasTangents = false;
            Vector3 minPos(std::numeric_limits<float>::max());
            Vector3 maxPos(std::numeric_limits<float>::lowest());

            bool decodedDraco = false;
            if (primitiveUsesDraco(primitive)) {
                counters.dracoPrimitiveCount++;
                decodedDraco = decodeDracoPrimitive(model, primitive, vertices, parsedIndices, minPos, maxPos,
                    hasNormals, hasTangents, colors);
                if (!decodedDraco) {
                    counters.dracoDecodeFailureCount++;
                    spdlog::warn("Skipping glTF primitive due to Draco decode failure (mesh={})", meshIndex);
                    return false;
                }
                counters.dracoDecodeSuccessCount++;
                // A Draco payload is always a triangle list.
                mode = TINYGLTF_MODE_TRIANGLES;
            }

            if (!decodedDraco) {
                const auto* positionAccessor = readablePositions(model, primitive);
                if (!positionAccessor) {
                    return false;
                }
                const AccessorReader positions(model, *positionAccessor, TINYGLTF_TYPE_VEC3);
                if (!positions.valid()) {
                    spdlog::warn("Skipping glTF primitive with unreadable POSITION data (mesh={})", meshIndex);
                    return false;
                }
                const auto vertexCount = positions.count();
                // An attribute that cannot be read, or covers fewer vertices than
                // POSITION, counts as absent.
                const auto attribute = [&](const char* name, const int type) {
                    const auto* accessor = primitiveAttribute(model, primitive, name);
                    if (!accessor) {
                        return AccessorReader();
                    }
                    AccessorReader reader(model, *accessor, type);
                    if (!reader.valid() || reader.count() < vertexCount) {
                        return AccessorReader();
                    }
                    return reader;
                };
                const AccessorReader normals = attribute("NORMAL", TINYGLTF_TYPE_VEC3);
                const AccessorReader uvs = attribute("TEXCOORD_0", TINYGLTF_TYPE_VEC2);
                const AccessorReader uvs1 = attribute("TEXCOORD_1", TINYGLTF_TYPE_VEC2);
                const AccessorReader tangents = attribute("TANGENT", TINYGLTF_TYPE_VEC4);
                hasNormals = normals.valid();
                hasTangents = tangents.valid();

                // COLOR_0 is VEC3 or VEC4; a VEC3 colour reads with alpha 1.
                AccessorReader colorReader = attribute("COLOR_0", TINYGLTF_TYPE_VEC4);
                if (!colorReader.valid()) {
                    colorReader = attribute("COLOR_0", TINYGLTF_TYPE_VEC3);
                }
                if (colorReader.valid()) {
                    colors.assign(vertexCount * 4, 1.0f);
                }

                vertices.resize(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i) {
                    float pos[3] = {0.0f, 0.0f, 0.0f};
                    positions.read(i, pos, 3);
                    // Overwritten by generated face normals when the file has none.
                    float normal[3] = {0.0f, 1.0f, 0.0f};
                    normals.read(i, normal, 3);
                    // glTF UVs are authored for GL-style sampling conventions. Texture
                    // sampling here uses a top-left origin, so V is flipped.
                    float uv[2] = {0.0f, 0.0f};
                    if (uvs.read(i, uv, 2)) {
                        uv[1] = 1.0f - uv[1];
                    }
                    float uv1[2] = {uv[0], uv[1]};
                    if (uvs1.read(i, uv1, 2)) {
                        uv1[1] = 1.0f - uv1[1];
                    }
                    // Leave the tangent zero when the file carries none; triangle
                    // primitives get one generated below (generateTangents), and the
                    // shaders skip normal mapping on a degenerate tangent rather than
                    // building a bogus fixed basis. There is no derivative-based TBN
                    // fallback in this port.
                    float tangent[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    if (tangents.read(i, tangent, 4)) {
                        // V is flipped above, so an imported tangent flips handedness.
                        tangent[3] = -tangent[3];
                    }
                    if (!colors.empty()) {
                        colorReader.read(i, colors.data() + i * 4, colorReader.components());
                    }
                    vertices[i] = PackedVertex{
                        pos[0], pos[1], pos[2],
                        normal[0], normal[1], normal[2],
                        uv[0], uv[1],
                        tangent[0], tangent[1], tangent[2], tangent[3],
                        uv1[0], uv1[1]
                    };
                    const Vector3 position = Vector3::load(pos);
                    minPos = Vector3::min(minPos, position);
                    maxPos = Vector3::max(maxPos, position);
                }

                if (primitive.indices >= 0) {
                    if (const auto* indexAccessor = getAccessor(model, primitive.indices)) {
                        readIndices(model, *indexAccessor, parsedIndices);
                    }
                }
            }

            if (vertices.empty()) {
                return false;
            }
            const size_t sourceVertexCount = vertices.size();

            // Flat shading for a triangle primitive without normals, then tangents for
            // one without tangents, derived from the normals the vertices now carry.
            std::vector<uint32_t> unweldSource;
            if (!hasNormals && isTriangleMode(mode)) {
                unweldSource = applyFlatNormals(vertices, parsedIndices, mode);
                if (vertices.empty()) {
                    return false;
                }
                remapPerVertex(colors, unweldSource, sourceVertexCount, 4);
            }
            if (!hasTangents && mode == TINYGLTF_MODE_TRIANGLES) {
                generateTangents(vertices, parsedIndices);
            }
            pd.mode = mode;

            // GPU skinning: JOINTS_0/WEIGHTS_0 present → the 88-byte skinned layout
            // (the Draco path never carries skin attributes here).
            pd.vertexCount = static_cast<int>(vertices.size());
            auto skinAttributes = decodedDraco
                ? SkinAttributes{} : readSkinAttributes(model, primitive, sourceVertexCount);
            if (skinAttributes.valid && !unweldSource.empty()) {
                remapPerVertex(skinAttributes.weights, unweldSource, sourceVertexCount, 4);
                remapPerVertex(skinAttributes.joints, unweldSource, sourceVertexCount, 4);
            }

            // Morph targets (skipped for Draco primitives — vertex order differs).
            if (!decodedDraco && !primitive.targets.empty()) {
                pd.morphTargets = readMorphTargets(model, primitive, sourceVertexCount);
                pd.morphInitialWeights.assign(mesh.weights.begin(), mesh.weights.end());
                for (auto& target : pd.morphTargets) {
                    remapPerVertex(target.deltaPositions, unweldSource, sourceVertexCount, 3);
                    remapPerVertex(target.deltaNormals, unweldSource, sourceVertexCount, 3);
                }
            }

            bool coloursMatter = false;
            for (size_t i = 0; i + 3 < colors.size() && !coloursMatter; i += 4) {
                coloursMatter = colors[i] != 1.0f || colors[i + 1] != 1.0f || colors[i + 2] != 1.0f;
            }
            if (coloursMatter && (skinAttributes.valid || !pd.morphTargets.empty())) {
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true)) {
                    spdlog::warn("GLB: COLOR_0 on a skinned or morphed primitive is not supported and is "
                        "ignored (mesh={}); further cases are not reported", meshIndex);
                }
            }

            if (skinAttributes.valid) {
                pd.skinned = true;
                pd.vertexBytes = packSkinnedVertices(vertices, skinAttributes);
            } else if (coloursMatter && pd.morphTargets.empty()) {
                pd.vertexColors = true;
                pd.vertexBytes = packColoredVertices(vertices, colors);
            } else {
                pd.vertexBytes.resize(vertices.size() * sizeof(PackedVertex));
                std::memcpy(pd.vertexBytes.data(), vertices.data(), pd.vertexBytes.size());
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
        // All of them at once, one per thread: decoding is most of the time a textured
        // model takes to load, and no image depends on another.
        result.images.resize(model.images.size());
        parallelFor(model.images.size(), [&](const size_t i) {
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
                return;
            }
            if (srcImage.as_is) {
                // Left encoded by loadImageData.
                decodeEncodedImage(srcImage, i, img);
                return;
            }
            // Pixels someone else decoded (a model built in memory).
            img.valid = buildRgba8Image(srcImage, img.rgbaPixels);
            if (img.valid) {
                img.width  = srcImage.width;
                img.height = srcImage.height;
            } else {
                spdlog::warn("glTF image '{}' unsupported format (bits={}, components={}, pixelType={})",
                    srcImage.name, srcImage.bits, srcImage.component, srcImage.pixel_type);
            }
        });

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
        result.meshSplats.resize(model.meshes.size());
        for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
            const auto& mesh = model.meshes[meshIndex];
            auto& primResults = result.meshPrimitives[meshIndex];

            for (const auto& primitive : mesh.primitives) {
                // A splat primitive is POINTS too, but it is a gaussian splat set, not a
                // point cloud: it never reaches the point path or the merge.
                if (isGaussianSplatPrimitive(primitive)) {
                    if (auto splats = gaussianSplatData(model, primitive, debugName)) {
                        result.meshSplats[meshIndex].push_back(std::move(splats));
                    }
                    continue;
                }
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
        auto coloredElements = VertexFormat::standardElements();
        coloredElements.push_back({VertexSemantic::SEMANTIC_COLOR, VertexDataType::TYPE_FLOAT32, 4,
            static_cast<uint32_t>(sizeof(PackedVertex))});
        const auto coloredVertexFormat = std::make_shared<VertexFormat>(
            static_cast<int>(COLORED_VERTEX_STRIDE), std::move(coloredElements), true, false);

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

        // A primitive with vertex colours draws with a COPY of its material that
        // compiles the vertex-colour variant: the variant reads the colour attribute,
        // which a mesh without one does not have, so it cannot go on the shared
        // material. One copy per source material, shared by every coloured primitive
        // using it. An invalid index falls back to the first material.
        std::vector<std::shared_ptr<Material>> vertexColorMaterials(gltfMaterials.size());
        const auto primitiveMaterial = [&](const int materialIndex, const bool vertexColors) {
            const size_t index = (materialIndex >= 0 && materialIndex < static_cast<int>(gltfMaterials.size()))
                ? static_cast<size_t>(materialIndex) : 0;
            if (!vertexColors) {
                return gltfMaterials[index];
            }
            auto& copy = vertexColorMaterials[index];
            if (!copy) {
                auto material = std::make_shared<StandardMaterial>(
                    *std::static_pointer_cast<StandardMaterial>(gltfMaterials[index]));
                material->setShaderVariantKey(material->shaderVariantKey() | (1ull << 21));  // VT_FEATURE_VERTEX_COLORS
                copy = material;
            }
            return copy;
        };

        // ── Meshes ───────────────────────────────────────────────────
        std::vector<std::vector<size_t>> meshToPayloadIndices(model.meshes.size());
        size_t nextPayloadIndex = 0;
        for (size_t meshIndex = 0; meshIndex < prepared.meshPrimitives.size(); ++meshIndex) {
            for (auto& pd : prepared.meshPrimitives[meshIndex]) {
                if (pd.vertexBytes.empty()) {
                    continue;
                }
                const auto& format = pd.pointCloud ? pointVertexFormat
                    : pd.skinned ? skinnedVertexFormat
                    : pd.vertexColors ? coloredVertexFormat : vertexFormat;
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
                    payload.material = primitiveMaterial(pd.materialIndex, pd.vertexColors);
                }
                for (const auto& [variant, materialIndex] : pd.variantMaterials) {
                    if (materialIndex >= 0 && materialIndex < static_cast<int>(gltfMaterials.size())) {
                        payload.variantMaterials[variant] = primitiveMaterial(materialIndex, pd.vertexColors);
                    }
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
                    [](const tinygltf::Primitive& p) {
                        return p.mode == TINYGLTF_MODE_POINTS && !isGaussianSplatPrimitive(p);
                    });
            }
        }

        // KHR_gaussian_splatting: the decoded splat sets become GPU resources here, one
        // per primitive, shared by every node that uses the mesh.
        std::vector<std::vector<std::shared_ptr<GSplatResource>>> meshSplatResources(model.meshes.size());
        for (size_t meshIndex = 0; meshIndex < prepared.meshSplats.size() && meshIndex < model.meshes.size(); ++meshIndex) {
            for (auto& data : prepared.meshSplats[meshIndex]) {
                if (data && device) {
                    meshSplatResources[meshIndex].push_back(std::make_shared<GSplatResource>(std::move(data), device));
                }
            }
        }

        // KHR_materials_variants: the variant names, by index.
        if (const auto ext = model.extensions.find("KHR_materials_variants");
            ext != model.extensions.end() && ext->second.IsObject() && ext->second.Has("variants")) {
            std::vector<std::string> names;
            const auto& variants = ext->second.Get("variants");
            for (size_t v = 0; variants.IsArray() && v < variants.ArrayLen(); ++v) {
                const auto& variant = variants.Get(static_cast<int>(v));
                names.push_back(variant.IsObject() && variant.Has("name") && variant.Get("name").IsString()
                    ? variant.Get("name").Get<std::string>() : "variant_" + std::to_string(v));
            }
            container->setMaterialVariants(std::move(names));
        }

        // ── Nodes: the glTF hierarchy and local transforms ───────────
        const std::vector<std::string> nodeNames = glbNodeNames(model, glbNodeParents(model));
        for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex) {
            const auto& node = model.nodes[nodeIndex];
            GlbNodePayload nodePayload;
            // Never empty: an unnamed node is `node_<index>`, and a repeated sibling
            // name is made unique; the animation channels and skin entries carry the
            // same names (glbNodeNames).
            nodePayload.name = nodeNames[nodeIndex];
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
            if (node.mesh >= 0 && node.mesh < static_cast<int>(meshSplatResources.size())) {
                nodePayload.splats = meshSplatResources[static_cast<size_t>(node.mesh)];
            }
            if (node.camera >= 0 && node.camera < static_cast<int>(model.cameras.size())) {
                nodePayload.camera = gltfCameraPayload(model.cameras[static_cast<size_t>(node.camera)]);
            }
            if (node.light >= 0 && node.light < static_cast<int>(model.lights.size())) {
                nodePayload.light = gltfLightPayload(model.lights[static_cast<size_t>(node.light)]);
            }
            if (const auto matrices = gltfInstanceMatrices(model, node); !matrices.empty() && device &&
                !nodePayload.meshPayloadIndices.empty()) {
                const int count = static_cast<int>(matrices.size() / 16);
                VertexBufferOptions options;
                options.data.resize(matrices.size() * sizeof(float));
                std::memcpy(options.data.data(), matrices.data(), options.data.size());
                nodePayload.instanceBuffer = device->createVertexBuffer(VertexFormat::defaultInstancingFormat(), count, options);
                nodePayload.instanceCount = nodePayload.instanceBuffer ? count : 0;
            }
            // A node that carries a camera or a light is not a disposable points leaf.
            if (nodePayload.camera || nodePayload.light) {
                nodePayload.skip = false;
            }
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
