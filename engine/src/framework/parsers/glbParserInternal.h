// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// Internal to the glTF parser: the helpers its translation units (glbParser*.cpp) share.
// Not part of the engine's interface; include it from those files only.
#pragma once

#include <tiny_gltf.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/math/matrix4.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "framework/parsers/glbParser.h"
#include "framework/parsers/packedVertex.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "scene/materials/standardMaterial.h"

namespace visutwin::canvas::gltf
{

        struct PackedPointVertex
        {
            float px, py, pz;       // position
            float cr, cg, cb, ca;   // vertex color (RGBA)
        };

        const tinygltf::Accessor* getAccessor(const tinygltf::Model& model, const int accessorIndex);

        int componentBytes(const int componentType);

        int accessorStride(const tinygltf::Model& model, const tinygltf::Accessor& accessor);

        std::vector<int> glbNodeParents(const tinygltf::Model& model);

        std::vector<std::string> glbNodeNames(const tinygltf::Model& model, const std::vector<int>& parents);

        std::string glbNodePath(const std::vector<std::string>& names, const std::vector<int>& parents, int nodeIndex);

        const uint8_t* getAccessorBase(const tinygltf::Model& model, const tinygltf::Accessor& accessor);

        float decodeComponent(const uint8_t* element, const int componentType,
            const bool normalized, const int component);


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

        bool readFloatArray(const tinygltf::Model& model, const tinygltf::Accessor& accessor, std::vector<float>& out);


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
            const tinygltf::Primitive& primitive, const size_t vertexCount);

        std::vector<uint8_t> packSkinnedVertices(const std::vector<PackedVertex>& vertices,
            const SkinAttributes& skin);

        std::vector<MorphTarget> readMorphTargets(const tinygltf::Model& model,
            const tinygltf::Primitive& primitive, const size_t vertexCount);

        void parseSkins(const tinygltf::Model& model, GlbContainerResource* container);

        void parseAnimations(const tinygltf::Model& model,
            AnimTrackList& outTracks);

        bool readIndices(const tinygltf::Model& model, const tinygltf::Accessor& accessor, std::vector<uint32_t>& out);

        bool buildRgba8Image(const tinygltf::Image& image, std::vector<uint8_t>& outRgba);

        bool imageHoldsKtx2(const tinygltf::Image& image);

        void decodeEncodedImage(const tinygltf::Image& image, const size_t imageIndex,
            PreparedGlbData::ImageData& out);


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

        void decomposeNodeMatrix(const std::vector<double>& matrix, Vector3& outT, Quaternion& outR, Vector3& outS);

        std::vector<float> gltfInstanceMatrices(const tinygltf::Model& model, const tinygltf::Node& node);

        std::optional<std::string> withoutMeshoptFallbackBuffers(const std::string_view json, const bool binary);

        std::optional<std::vector<std::uint8_t>> withGlbJson(const std::uint8_t* data, const size_t length,
            const std::string& json);

        void decodeMeshoptBufferViews(tinygltf::Model& model, const std::string& debugName);

        std::shared_ptr<StandardMaterial> createGltfMaterial(const tinygltf::Material& srcMaterial,
            const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture, size_t* textureCount = nullptr);

        std::shared_ptr<StandardMaterial> createDefaultGltfMaterial();

        const tinygltf::Accessor* primitiveAttribute(const tinygltf::Model& model,
            const tinygltf::Primitive& primitive, const char* name);

        bool isGaussianSplatPrimitive(const tinygltf::Primitive& primitive);

        std::unique_ptr<GSplatData> gaussianSplatData(const tinygltf::Model& model,
            const tinygltf::Primitive& primitive, const std::string& source);

        const tinygltf::Accessor* readablePositions(const tinygltf::Model& model, const tinygltf::Primitive& primitive);

        int textureImageSource(const tinygltf::Texture& texture);


        // A glTF texture's sampling state, resolved: -1 leaves a filter at the texture's
        // default, and an omitted sampler wraps as the glTF default (REPEAT). Two glTF
        // textures with the same image and an equal state share one Texture.
        struct GltfSamplerState
        {
            int minFilter = -1;
            int magFilter = -1;
            AddressMode addressU = ADDRESS_REPEAT;
            AddressMode addressV = ADDRESS_REPEAT;

            bool operator<(const GltfSamplerState& other) const
            {
                return std::tie(minFilter, magFilter, addressU, addressV) <
                    std::tie(other.minFilter, other.magFilter, other.addressU, other.addressV);
            }
        };

        GltfSamplerState resolveGltfSampler(const tinygltf::Model& model, const tinygltf::Texture& srcTexture);

        void applyGltfSampler(Texture& texture, const GltfSamplerState& state);

        std::vector<bool> gltfColourTextures(const tinygltf::Model& model);

        std::shared_ptr<Texture> createPreparedTexture(const PreparedGlbData::ImageData& image,
            const std::string& name, const std::shared_ptr<GraphicsDevice>& device);

        std::vector<Matrix4> computeNodeWorldMatrices(const tinygltf::Model& model);

        void appendPointVertices(const tinygltf::Model& model, const tinygltf::Accessor& positions,
            const tinygltf::Accessor* colors, const Matrix4* transform,
            std::vector<PackedPointVertex>& out, Vector3& boundsMin, Vector3& boundsMax);

        PreparedGlbData::PrimitiveData pointCloudData(std::vector<PackedPointVertex>&& vertices,
            const Vector3& boundsMin, const Vector3& boundsMax, const int materialIndex);

        std::vector<std::pair<int, int>> primitiveVariantMaterials(const tinygltf::Primitive& primitive);

        bool extractTrianglePrimitive(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
            const tinygltf::Primitive& primitive, const size_t meshIndex, PreparedGlbData& counters,
            PreparedGlbData::PrimitiveData& pd);

        std::shared_ptr<Mesh> createPreparedMesh(PreparedGlbData::PrimitiveData& pd,
            const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<VertexFormat>& format);

        std::shared_ptr<Material> pointCloudMaterial(const std::vector<std::shared_ptr<Material>>& materials,
            const int materialIndex, const bool animated);
}
