// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// What the glTF parser builds from a primitive's attributes and from two material
// extensions, on both model load paths (createFromModel, and prepareFromModel +
// createFromPrepared). Every model is built in memory.
//   - A triangle primitive without NORMAL is FLAT shaded: each triangle gets three
//     vertices of its own with its face normal (strips and fans become lists), and
//     the derived tangents are perpendicular to those normals. Before, every vertex
//     was lit with (0, 1, 0).
//   - Sparse accessors apply to vertex attributes: overrides over a base view, a
//     base-less accessor (zeros plus overrides), and quantised sparse values. Before,
//     only the base view was read, and a base-less POSITION read nothing.
//   - KHR_materials_pbrSpecularGlossiness sets opacity only; the file's alphaMode
//     decides blending.
//   - KHR_materials_clearcoat's clearcoatNormalTexture.scale is the coat bumpiness.
//   - COLOR_0 on a static triangle primitive takes the 72-byte vertex-coloured layout
//     and a vertex-colour copy of the material; a white colour stream, and a colour on
//     a morphed primitive, keep the 56-byte layout.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "framework/parsers/glbContainerResource.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "support/check.h"
#include "support/gltfModel.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    using Value = tinygltf::Value;

    constexpr float kTolerance = 1e-5f;
    constexpr uint64_t kVertexColorsBit = 1ull << 21;

    // Appends raw bytes (padded to 4) to buffer 0 behind a view of their own; returns
    // the view's index.
    int addView(tinygltf::Model& model, std::vector<unsigned char>& bytes, const void* data, const size_t size)
    {
        const size_t at = bytes.size();
        bytes.resize(at + ((size + 3) & ~size_t{3}));
        std::memcpy(bytes.data() + at, data, size);
        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = at;
        view.byteLength = size;
        model.bufferViews.push_back(view);
        return static_cast<int>(model.bufferViews.size()) - 1;
    }

    // One mesh of the given primitives under one node, one scene, and `materials`
    // (none: the parser's default material).
    tinygltf::Model finishModel(tinygltf::Model model, tinygltf::Buffer buffer,
        const std::vector<tinygltf::Primitive>& primitives, const std::vector<tinygltf::Material>& materials = {})
    {
        model.asset.version = "2.0";
        model.buffers.push_back(std::move(buffer));
        tinygltf::Mesh mesh;
        mesh.name = "mesh";
        mesh.primitives = primitives;
        model.meshes.push_back(mesh);
        tinygltf::Node node;
        node.name = "Node";
        node.mesh = 0;
        model.nodes.push_back(node);
        tinygltf::Scene scene;
        scene.nodes = {0};
        model.scenes.push_back(scene);
        model.defaultScene = 0;
        model.materials = materials;
        return model;
    }

    // The floats of payload `index`'s vertex buffer, `stride` floats per vertex.
    std::vector<float> vertexFloats(const GlbContainerResource& container, const size_t index)
    {
        const auto vb = container.meshPayloads()[index].mesh->getVertexBuffer();
        std::vector<float> floats(vb->storage().size() / sizeof(float));
        std::memcpy(floats.data(), vb->storage().data(), floats.size() * sizeof(float));
        return floats;
    }

    int vertexStride(const GlbContainerResource& container, const size_t index)
    {
        const auto vb = container.meshPayloads()[index].mesh->getVertexBuffer();
        return vb && vb->format() ? vb->format()->size() : 0;
    }

    std::string label(const LoadPath path, const std::string& what)
    {
        return std::string(path == LoadPath::CreateFromModel ? "createFromModel: " : "createFromPrepared: ") + what;
    }

    // Runs `visit` on each load path's container, checking it has `payloads` payloads.
    void forEachContainer(const std::function<tinygltf::Model()>& build, const std::shared_ptr<GraphicsDevice>& device,
        const size_t payloads, const std::function<void(const GlbContainerResource&, LoadPath)>& visit)
    {
        forEachLoadPath(build, device, "glbVertexAttributeTests",
            [&](const GlbContainerResource* container, const LoadPath path) {
                const bool built = container && container->meshPayloads().size() == payloads;
                check(built, label(path, "builds the expected mesh payloads"));
                if (built) {
                    visit(*container, path);
                }
            });
    }

    // ── Flat normals ─────────────────────────────────────────────────

    // Four vertices, two indexed triangles in different planes sharing the edge v0-v2:
    // (v0, v1, v2) faces +Z and (v0, v2, v3) faces +X. UVs so tangents can be derived.
    tinygltf::Model indexedNoNormals()
    {
        tinygltf::Model model;
        tinygltf::Buffer buffer;
        tinygltf::Primitive primitive;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        primitive.attributes["POSITION"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f}, 4, TINYGLTF_TYPE_VEC3);
        primitive.attributes["TEXCOORD_0"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  1.0f, 1.0f}, 4, TINYGLTF_TYPE_VEC2);
        const uint16_t indices[] = {0, 1, 2,  0, 2, 3};
        const int view = addView(model, buffer.data, indices, sizeof(indices));
        tinygltf::Accessor accessor;
        accessor.bufferView = view;
        accessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
        accessor.count = 6;
        accessor.type = TINYGLTF_TYPE_SCALAR;
        model.accessors.push_back(accessor);
        primitive.indices = static_cast<int>(model.accessors.size()) - 1;
        return finishModel(std::move(model), std::move(buffer), {primitive});
    }

    // A planar strip of four vertices in the XY plane: both triangles face +Z.
    tinygltf::Model stripNoNormals()
    {
        tinygltf::Model model;
        tinygltf::Buffer buffer;
        tinygltf::Primitive primitive;
        primitive.mode = TINYGLTF_MODE_TRIANGLE_STRIP;
        primitive.attributes["POSITION"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f, 0.0f}, 4, TINYGLTF_TYPE_VEC3);
        return finishModel(std::move(model), std::move(buffer), {primitive});
    }

    // A non-indexed triangle facing +Z, with and without its own NORMAL (which points
    // the other way, to tell a kept normal from a generated one).
    tinygltf::Model listTriangle(const bool withNormals)
    {
        tinygltf::Model model;
        tinygltf::Buffer buffer;
        tinygltf::Primitive primitive;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        primitive.attributes["POSITION"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 0.0f,  2.0f, 0.0f, 0.0f,  0.0f, 2.0f, 0.0f}, 3, TINYGLTF_TYPE_VEC3);
        if (withNormals) {
            primitive.attributes["NORMAL"] = addAccessor(model, buffer.data,
                {0.0f, 0.0f, -1.0f,  0.0f, 0.0f, -1.0f,  0.0f, 0.0f, -1.0f}, 3, TINYGLTF_TYPE_VEC3);
        }
        return finishModel(std::move(model), std::move(buffer), {primitive});
    }

    Vector3 normalOf(const std::vector<float>& v, const size_t vertex)
    {
        return Vector3(v[vertex * 14 + 3], v[vertex * 14 + 4], v[vertex * 14 + 5]);
    }

    void flatNormalTests(const std::shared_ptr<GraphicsDevice>& device)
    {
        std::cout << "flat normals\n";
        forEachContainer(indexedNoNormals, device, 1, [](const GlbContainerResource& container, const LoadPath path) {
            const auto& mesh = *container.meshPayloads()[0].mesh;
            const Primitive primitive = mesh.getPrimitive();
            check(primitive.type == PRIMITIVE_TRIANGLES && !primitive.indexed && primitive.count == 6,
                label(path, "an indexed primitive without normals is unwelded into a 6-vertex triangle list"));
            const auto v = vertexFloats(container, 0);
            if (v.size() != 6 * 14) {
                fail(label(path, "six 56-byte vertices"));
                return;
            }
            bool facets = true;
            for (size_t i = 0; i < 6; ++i) {
                facets = facets && nearStrict(normalOf(v, i), i < 3 ? 0.0f : 1.0f, 0.0f, i < 3 ? 1.0f : 0.0f, kTolerance);
            }
            check(facets, label(path, "each triangle's vertices carry its face normal (+Z, then +X), not (0, 1, 0)"));
            check(nearStrict(Vector3(v[3 * 14], v[3 * 14 + 1], v[3 * 14 + 2]), 0.0f, 0.0f, 0.0f, kTolerance) &&
                  nearStrict(Vector3(v[5 * 14], v[5 * 14 + 1], v[5 * 14 + 2]), 0.0f, 0.0f, 1.0f, kTolerance),
                label(path, "the unwelded vertices follow the index order"));
            bool tangents = true;
            for (size_t i = 0; i < 6; ++i) {
                const Vector3 t(v[i * 14 + 8], v[i * 14 + 9], v[i * 14 + 10]);
                tangents = tangents && near(t.length(), 1.0f, 1e-4f) && std::abs(t.dot(normalOf(v, i))) < 1e-4f;
            }
            check(tangents, label(path, "the derived tangents are unit and perpendicular to the generated normals"));
        });

        forEachContainer(stripNoNormals, device, 1, [](const GlbContainerResource& container, const LoadPath path) {
            const Primitive primitive = container.meshPayloads()[0].mesh->getPrimitive();
            check(primitive.type == PRIMITIVE_TRIANGLES && primitive.count == 6,
                label(path, "a strip without normals becomes a 6-vertex triangle list"));
            const auto v = vertexFloats(container, 0);
            bool facing = v.size() == 6 * 14;
            for (size_t i = 0; facing && i < 6; ++i) {
                facing = nearStrict(normalOf(v, i), 0.0f, 0.0f, 1.0f, kTolerance);
            }
            check(facing, label(path, "both strip triangles face +Z (the odd one is not flipped)"));
        });

        forEachContainer([] { return listTriangle(false); }, device, 1,
            [](const GlbContainerResource& container, const LoadPath path) {
                const auto v = vertexFloats(container, 0);
                bool flat = v.size() == 3 * 14;
                for (size_t i = 0; flat && i < 3; ++i) {
                    flat = nearStrict(normalOf(v, i), 0.0f, 0.0f, 1.0f, kTolerance);
                }
                check(flat, label(path, "a non-indexed triangle without normals keeps its 3 vertices, now facing +Z"));
            });

        forEachContainer([] { return listTriangle(true); }, device, 1,
            [](const GlbContainerResource& container, const LoadPath path) {
                const auto v = vertexFloats(container, 0);
                check(v.size() == 3 * 14 && nearStrict(normalOf(v, 0), 0.0f, 0.0f, -1.0f, kTolerance),
                    label(path, "a file's own normals are kept"));
            });
    }

    // ── Sparse vertex attributes ─────────────────────────────────────

    // Writes a sparse block onto `accessor`: uint16 indices and the raw value bytes.
    void makeSparse(tinygltf::Model& model, tinygltf::Buffer& buffer, tinygltf::Accessor& accessor,
        const std::vector<uint16_t>& indices, const void* values, const size_t valueBytes)
    {
        accessor.sparse.isSparse = true;
        accessor.sparse.count = static_cast<int>(indices.size());
        accessor.sparse.indices.bufferView = addView(model, buffer.data, indices.data(), indices.size() * sizeof(uint16_t));
        accessor.sparse.indices.byteOffset = 0;
        accessor.sparse.indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
        accessor.sparse.values.bufferView = addView(model, buffer.data, values, valueBytes);
        accessor.sparse.values.byteOffset = 0;
    }

    // A triangle with normals (so nothing is unwelded) whose POSITION is sparse: over
    // a base view (vertex 1 moved to (5, 0, 0)), or base-less (vertex 0 stays at zero,
    // vertices 1 and 2 come from the overrides). The base-less case also carries a
    // base-less TEXCOORD_0 of normalized uint16 with one override.
    tinygltf::Model sparsePositions(const bool baseless)
    {
        tinygltf::Model model;
        tinygltf::Buffer buffer;
        tinygltf::Primitive primitive;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;

        int positions = -1;
        if (baseless) {
            tinygltf::Accessor accessor;
            accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
            accessor.count = 3;
            accessor.type = TINYGLTF_TYPE_VEC3;
            const float values[] = {3.0f, 0.0f, 0.0f,  0.0f, 4.0f, 0.0f};
            makeSparse(model, buffer, accessor, {1, 2}, values, sizeof(values));
            model.accessors.push_back(accessor);
            positions = static_cast<int>(model.accessors.size()) - 1;
        } else {
            positions = addAccessor(model, buffer.data,
                {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f}, 3, TINYGLTF_TYPE_VEC3);
            const float values[] = {5.0f, 0.0f, 0.0f};
            makeSparse(model, buffer, model.accessors[static_cast<size_t>(positions)], {1}, values, sizeof(values));
        }
        primitive.attributes["POSITION"] = positions;
        primitive.attributes["NORMAL"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f}, 3, TINYGLTF_TYPE_VEC3);

        if (baseless) {
            tinygltf::Accessor uv;
            uv.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
            uv.normalized = true;
            uv.count = 3;
            uv.type = TINYGLTF_TYPE_VEC2;
            const uint16_t values[] = {65535, 0};
            makeSparse(model, buffer, uv, {2}, values, sizeof(values));
            model.accessors.push_back(uv);
            primitive.attributes["TEXCOORD_0"] = static_cast<int>(model.accessors.size()) - 1;
        }
        return finishModel(std::move(model), std::move(buffer), {primitive});
    }

    Vector3 positionOf(const std::vector<float>& v, const size_t vertex)
    {
        return Vector3(v[vertex * 14], v[vertex * 14 + 1], v[vertex * 14 + 2]);
    }

    void sparseTests(const std::shared_ptr<GraphicsDevice>& device)
    {
        std::cout << "\nsparse vertex attributes\n";
        forEachContainer([] { return sparsePositions(false); }, device, 1,
            [](const GlbContainerResource& container, const LoadPath path) {
                const auto v = vertexFloats(container, 0);
                check(v.size() == 3 * 14 && nearStrict(positionOf(v, 0), 0.0f, 0.0f, 0.0f, kTolerance) &&
                      nearStrict(positionOf(v, 1), 5.0f, 0.0f, 0.0f, kTolerance) &&
                      nearStrict(positionOf(v, 2), 0.0f, 1.0f, 0.0f, kTolerance),
                    label(path, "a sparse override replaces its vertex over the base view"));
                const BoundingBox& box = container.meshPayloads()[0].mesh->aabb();
                check(nearStrict(box.center() + box.halfExtents(), 5.0f, 1.0f, 0.0f, kTolerance),
                    label(path, "the bounds include the overridden vertex"));
            });

        forEachContainer([] { return sparsePositions(true); }, device, 1,
            [](const GlbContainerResource& container, const LoadPath path) {
                const auto v = vertexFloats(container, 0);
                check(v.size() == 3 * 14 && nearStrict(positionOf(v, 0), 0.0f, 0.0f, 0.0f, kTolerance) &&
                      nearStrict(positionOf(v, 1), 3.0f, 0.0f, 0.0f, kTolerance) &&
                      nearStrict(positionOf(v, 2), 0.0f, 4.0f, 0.0f, kTolerance),
                    label(path, "a base-less POSITION is zeros plus its overrides"));
                const BoundingBox& box = container.meshPayloads()[0].mesh->aabb();
                check(nearStrict(box.center() - box.halfExtents(), 0.0f, 0.0f, 0.0f, kTolerance) &&
                      nearStrict(box.center() + box.halfExtents(), 3.0f, 4.0f, 0.0f, kTolerance),
                    label(path, "and its bounds are finite and exact"));
                if (v.size() == 3 * 14) {
                    // V is flipped on load: v' = 1 - v.
                    check(nearStrict(v[6], 0.0f, kTolerance) && nearStrict(v[7], 1.0f, kTolerance) &&
                          nearStrict(v[2 * 14 + 6], 1.0f, kTolerance) && nearStrict(v[2 * 14 + 7], 1.0f, kTolerance),
                        label(path, "a base-less normalized uint16 TEXCOORD_0 de-quantises its override"));
                }
            });
    }

    // ── Material extensions ──────────────────────────────────────────

    Value numberArray(const std::vector<double>& values)
    {
        Value::Array array;
        for (const double value : values) {
            array.emplace_back(value);
        }
        return Value(array);
    }

    tinygltf::Material specGlossMaterial(const std::string& alphaMode, const double alpha)
    {
        tinygltf::Material material;
        material.name = alphaMode;
        material.alphaMode = alphaMode;
        Value::Object sg;
        sg["diffuseFactor"] = numberArray({0.5, 0.5, 0.5, alpha});
        material.extensions["KHR_materials_pbrSpecularGlossiness"] = Value(sg);
        return material;
    }

    tinygltf::Material clearcoatMaterial(const bool withScale)
    {
        tinygltf::Material material;
        material.name = withScale ? "scaled" : "unscaled";
        Value::Object normalInfo;
        normalInfo["index"] = Value(0);
        if (withScale) {
            normalInfo["scale"] = Value(0.25);
        }
        Value::Object cc;
        cc["clearcoatFactor"] = Value(1.0);
        cc["clearcoatNormalTexture"] = Value(normalInfo);
        material.extensions["KHR_materials_clearcoat"] = Value(cc);
        return material;
    }

    // One triangle (with normals) per material.
    tinygltf::Model trianglesWithMaterials(const std::vector<tinygltf::Material>& materials)
    {
        tinygltf::Model model;
        tinygltf::Buffer buffer;
        const int positions = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f}, 3, TINYGLTF_TYPE_VEC3);
        const int normals = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f}, 3, TINYGLTF_TYPE_VEC3);
        std::vector<tinygltf::Primitive> primitives;
        for (size_t i = 0; i < materials.size(); ++i) {
            tinygltf::Primitive primitive;
            primitive.mode = TINYGLTF_MODE_TRIANGLES;
            primitive.attributes["POSITION"] = positions;
            primitive.attributes["NORMAL"] = normals;
            primitive.material = static_cast<int>(i);
            primitives.push_back(primitive);
        }
        return finishModel(std::move(model), std::move(buffer), primitives, materials);
    }

    const StandardMaterial* standardMaterial(const GlbContainerResource& container, const size_t index)
    {
        return dynamic_cast<const StandardMaterial*>(container.meshPayloads()[index].material.get());
    }

    void materialTests(const std::shared_ptr<GraphicsDevice>& device)
    {
        std::cout << "\nmaterial extensions\n";
        forEachContainer([] {
                return trianglesWithMaterials({specGlossMaterial("BLEND", 1.0), specGlossMaterial("OPAQUE", 0.9)});
            }, device, 2, [](const GlbContainerResource& container, const LoadPath path) {
                const auto* blend = standardMaterial(container, 0);
                const auto* opaque = standardMaterial(container, 1);
                check(blend && blend->alphaMode() == AlphaMode::BLEND && blend->transparent() &&
                      blend->opacity() == 1.0f,
                    label(path, "spec-gloss BLEND with diffuse alpha 1 blends at opacity 1 (no glass substitute)"));
                check(opaque && opaque->alphaMode() == AlphaMode::OPAQUE && !opaque->transparent() &&
                      nearStrict(opaque->opacity(), 0.9f, kTolerance),
                    label(path, "spec-gloss OPAQUE with diffuse alpha 0.9 stays opaque"));
            });

        forEachContainer([] { return trianglesWithMaterials({clearcoatMaterial(true), clearcoatMaterial(false)}); },
            device, 2, [](const GlbContainerResource& container, const LoadPath path) {
                const auto* scaled = standardMaterial(container, 0);
                const auto* unscaled = standardMaterial(container, 1);
                check(scaled && scaled->clearCoatBumpiness() == 0.25f,
                    label(path, "clearcoatNormalTexture.scale is the clearcoat bumpiness"));
                check(unscaled && unscaled->clearCoatBumpiness() == 1.0f,
                    label(path, "an absent scale is 1"));
                // The reproduced upstream quirk: a quarter of clearcoatFactor (1 here).
                check(scaled && scaled->clearCoat() == 0.25f,
                    label(path, "the coat's strength is a quarter of clearcoatFactor"));
            });
    }

    // ── COLOR_0 ──────────────────────────────────────────────────────

    // Three triangles sharing material 0: one with a non-white COLOR_0, one without a
    // colour, one with a white COLOR_0; and a morphed triangle with a non-white colour.
    tinygltf::Model coloredTriangles()
    {
        tinygltf::Model model;
        tinygltf::Buffer buffer;
        const int positions = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f}, 3, TINYGLTF_TYPE_VEC3);
        const int normals = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f}, 3, TINYGLTF_TYPE_VEC3);
        const int colored = addAccessor(model, buffer.data,
            {0.25f, 0.5f, 1.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f}, 3, TINYGLTF_TYPE_VEC3);
        const int white = addAccessor(model, buffer.data,
            {1.0f, 1.0f, 1.0f, 0.0f,  1.0f, 1.0f, 1.0f, 0.0f,  1.0f, 1.0f, 1.0f, 0.0f}, 3, TINYGLTF_TYPE_VEC4);
        const int delta = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f}, 3, TINYGLTF_TYPE_VEC3);

        const auto triangle = [&](const int color, const bool morphed) {
            tinygltf::Primitive primitive;
            primitive.mode = TINYGLTF_MODE_TRIANGLES;
            primitive.material = 0;
            primitive.attributes["POSITION"] = positions;
            primitive.attributes["NORMAL"] = normals;
            if (color >= 0) {
                primitive.attributes["COLOR_0"] = color;
            }
            if (morphed) {
                std::map<std::string, int> target;
                target["POSITION"] = delta;
                primitive.targets.push_back(target);
            }
            return primitive;
        };
        tinygltf::Material material;
        material.name = "shared";
        return finishModel(std::move(model), std::move(buffer),
            {triangle(colored, false), triangle(-1, false), triangle(white, false), triangle(colored, true)},
            {material});
    }

    void colorTests(const std::shared_ptr<GraphicsDevice>& device)
    {
        std::cout << "\nCOLOR_0\n";
        forEachContainer(coloredTriangles, device, 4, [](const GlbContainerResource& container, const LoadPath path) {
            const auto& payloads = container.meshPayloads();
            check(vertexStride(container, 0) == 72, label(path, "a coloured static primitive takes the 72-byte layout"));
            const auto vb = payloads[0].mesh->getVertexBuffer();
            bool colorElement = false;
            for (const auto& element : vb->format()->elements()) {
                colorElement = colorElement || (element.semantic == VertexSemantic::SEMANTIC_COLOR &&
                    element.offset == 56 && element.componentCount == 4);
            }
            check(colorElement, label(path, "with a float4 COLOR element at offset 56"));

            const auto v = vertexFloats(container, 0);
            if (v.size() == 3 * 18) {
                // Stored LINEAR, exactly the file's values: a glTF material never
                // asks the vertex stage to decode gamma. Alpha is 1.
                check(v[14] == 0.25f && v[15] == 0.5f && v[16] == 1.0f && v[17] == 1.0f &&
                      v[18 + 14] == 1.0f && v[18 + 15] == 0.0f,
                    label(path, "the colour is the file's linear value, with alpha 1"));
            } else {
                fail(label(path, "three 72-byte vertices"));
            }

            const uint64_t colouredKey = payloads[0].material ? payloads[0].material->shaderVariantKey() : 0;
            const uint64_t plainKey = payloads[1].material ? payloads[1].material->shaderVariantKey() : 0;
            check((colouredKey & kVertexColorsBit) != 0, label(path, "the coloured primitive compiles vertex colours"));
            check(payloads[0].material != payloads[1].material && (plainKey & kVertexColorsBit) == 0,
                label(path, "through a copy: the shared material stays without them"));
            const auto* colouredStd = dynamic_cast<const StandardMaterial*>(payloads[0].material.get());
            check(colouredStd && !colouredStd->vertexColorGamma() && (colouredKey & (1ull << 35)) == 0,
                label(path, "and reads them as linear (no gamma decode)"));
            check(vertexStride(container, 1) == 56, label(path, "a primitive without COLOR_0 keeps 56 bytes"));
            check(vertexStride(container, 2) == 56 && payloads[2].material == payloads[1].material,
                label(path, "a white COLOR_0 is dropped: 56 bytes and the shared material"));
            check(vertexStride(container, 3) == 56 && payloads[3].morph &&
                  payloads[3].material && (payloads[3].material->shaderVariantKey() & kVertexColorsBit) == 0,
                label(path, "a morphed primitive ignores its COLOR_0"));
        });
    }
}

int main()
{
    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.cpuBuffers = true});
    flatNormalTests(device);
    sparseTests(device);
    materialTests(device);
    colorTests(device);
    return finish("GLB vertex attributes");
}
