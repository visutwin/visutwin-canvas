// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// glTF parser: accessor and buffer-view reading (de-quantisation, sparse), node names and
// transforms, instancing matrices.
#include "glbParserInternal.h"

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
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "core/shape/boundingBox.h"
#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "core/math/vector3.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/textureMips.h"
#include "platform/graphics/textureUtils.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    using namespace gltf;

    namespace gltf
    {
        const tinygltf::Accessor* getAccessor(const tinygltf::Model& model, const int accessorIndex)
        {
            if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) {
                return nullptr;
            }
            return &model.accessors[accessorIndex];
        }
    }

    namespace
    {
        const tinygltf::BufferView* getBufferView(const tinygltf::Model& model, const tinygltf::Accessor& accessor)
        {
            if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size())) {
                return nullptr;
            }
            return &model.bufferViews[accessor.bufferView];
        }
    }

    namespace gltf
    {
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
    }

    namespace
    {
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
    }

    namespace gltf
    {
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
        std::vector<float> gltfInstanceMatrices(const tinygltf::Model& model, const tinygltf::Node& node)
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
    }
}
