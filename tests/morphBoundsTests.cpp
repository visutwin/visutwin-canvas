// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A morph moves vertices past the rest pose, so the bounds that culling, light
// culling and the shadow fit read must grow by how far its targets reach — or a
// mesh whose targets push it outward is culled while still on screen. Until
// 2026-09-23 the expansion was a commented-out TODO in MeshInstance::aabb and the
// GLB bone boxes ignored morph targets altogether. Nothing in a render shows it
// unless a morph carries a mesh across a frustum edge, so these pin the numbers:
//   - Morph::aabb is the union of every target's DELTA bounds and the origin
//     (upstream Morph.aabb);
//   - a morphed mesh instance's local bounds grow by it (upstream _expand), and
//     attaching the morph invalidates bounds cached before it;
//   - a skinned, morphed glTF's bone box takes each vertex's reach under all of its
//     targets at once (upstream Mesh._initBoneAabbs).

#include <tiny_gltf.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/glbParser.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/graphNode.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/morph.h"
#include "scene/morphInstance.h"
#include "scene/skin.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    bool near(const Vector3& a, const float x, const float y, const float z)
    {
        return std::fabs(a.getX() - x) < 1e-5f && std::fabs(a.getY() - y) < 1e-5f &&
            std::fabs(a.getZ() - z) < 1e-5f;
    }

    Vector3 boxMin(const BoundingBox& box) { return box.center() - box.halfExtents(); }
    Vector3 boxMax(const BoundingBox& box) { return box.center() + box.halfExtents(); }

    std::string describe(const BoundingBox& box)
    {
        const Vector3 lo = boxMin(box);
        const Vector3 hi = boxMax(box);
        return "(" + std::to_string(lo.getX()) + ", " + std::to_string(lo.getY()) + ", " + std::to_string(lo.getZ()) +
            ") - (" + std::to_string(hi.getX()) + ", " + std::to_string(hi.getY()) + ", " + std::to_string(hi.getZ()) + ")";
    }

    /// Creates no GPU objects: the parser gets null buffers back and keeps going,
    /// and the bone bounds are computed from the glTF data before any of that.
    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>&, int,
            const VertexBufferOptions&) override { return nullptr; }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int, const std::vector<uint8_t>&) override
        {
            return nullptr;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

    // Two targets over a three-vertex mesh (rest positions (0,0,0), (1,0,0), (0,1,0)):
    //   A moves vertex 0 by (0, 0, -2);
    //   B moves vertex 0 by (0, 0, +1) and vertex 1 by (+3, 0, 0).
    std::vector<MorphTarget> twoTargets()
    {
        MorphTarget a;
        a.name = "A";
        a.deltaPositions = {0.0f, 0.0f, -2.0f,  0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f};
        MorphTarget b;
        b.name = "B";
        b.deltaPositions = {0.0f, 0.0f, 1.0f,  3.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f};
        return {a, b};
    }

    int addAccessor(tinygltf::Model& model, std::vector<unsigned char>& bytes, const void* data,
        const size_t size, const int count, const int type, const int componentType)
    {
        const size_t at = bytes.size();
        bytes.resize(at + size);
        std::memcpy(bytes.data() + at, data, size);

        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = at;
        view.byteLength = size;
        model.bufferViews.push_back(view);

        tinygltf::Accessor accessor;
        accessor.bufferView = static_cast<int>(model.bufferViews.size()) - 1;
        accessor.componentType = componentType;
        accessor.count = static_cast<size_t>(count);
        accessor.type = type;
        model.accessors.push_back(accessor);
        return static_cast<int>(model.accessors.size()) - 1;
    }

    // Node 0 "Body" carries the mesh and skin 0; node 1 "Bone" is its only joint.
    // Every vertex is fully weighted to that joint, so its bone box is the box of
    // every vertex's reach.
    tinygltf::Model skinnedMorphedModel()
    {
        tinygltf::Model model;
        model.asset.version = "2.0";
        model.nodes.resize(2);
        model.nodes[0].name = "Body";
        model.nodes[0].mesh = 0;
        model.nodes[0].skin = 0;
        model.nodes[0].children = {1};
        model.nodes[1].name = "Bone";

        tinygltf::Scene scene;
        scene.nodes = {0};
        model.scenes.push_back(scene);
        model.defaultScene = 0;

        tinygltf::Buffer buffer;
        const float positions[] = {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f};
        const uint16_t joints[] = {0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0};
        const float weights[] = {1.0f, 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f, 0.0f};
        const auto targets = twoTargets();

        tinygltf::Primitive primitive;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        primitive.attributes["POSITION"] = addAccessor(model, buffer.data, positions, sizeof(positions), 3,
            TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
        primitive.attributes["JOINTS_0"] = addAccessor(model, buffer.data, joints, sizeof(joints), 3,
            TINYGLTF_TYPE_VEC4, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
        primitive.attributes["WEIGHTS_0"] = addAccessor(model, buffer.data, weights, sizeof(weights), 3,
            TINYGLTF_TYPE_VEC4, TINYGLTF_COMPONENT_TYPE_FLOAT);
        for (const auto& target : targets) {
            std::map<std::string, int> attributes;
            attributes["POSITION"] = addAccessor(model, buffer.data, target.deltaPositions.data(),
                target.deltaPositions.size() * sizeof(float), 3, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
            primitive.targets.push_back(attributes);
        }
        model.buffers.push_back(buffer);

        tinygltf::Mesh mesh;
        mesh.name = "body";
        mesh.primitives.push_back(primitive);
        model.meshes.push_back(mesh);

        tinygltf::Skin skin;
        skin.joints = {1};
        model.skins.push_back(skin);
        return model;
    }
}

int main()
{
    std::cout << "morph delta bounds\n";
    {
        const Morph morph(twoTargets(), 3, nullptr);
        // Deltas: (0,0,-2), (0,0,1), (3,0,0) and zeros, with the origin included.
        check(near(boxMin(morph.aabb()), 0.0f, 0.0f, -2.0f), "min is the most negative delta per axis: " + describe(morph.aabb()));
        check(near(boxMax(morph.aabb()), 3.0f, 0.0f, 1.0f), "max is the most positive delta per axis");

        MorphTarget outward;
        outward.deltaPositions = {1.0f, 2.0f, 3.0f};
        const Morph onlyPositive({outward}, 1, nullptr);
        check(near(boxMin(onlyPositive.aabb()), 0.0f, 0.0f, 0.0f),
            "the origin is always inside: a target that only pushes outward keeps min at 0");
    }

    std::cout << "\nmorphed mesh instance bounds\n";
    {
        auto mesh = std::make_shared<Mesh>();
        BoundingBox rest;
        rest.setCenter(0.5f, 0.5f, 0.0f);
        rest.setHalfExtents(0.5f, 0.5f, 0.0f);   // (0,0,0) - (1,1,0), the triangle's rest box
        mesh->setAabb(rest);

        GraphNode node;
        MeshInstance instance(mesh.get(), nullptr, &node);
        const BoundingBox before = instance.aabb();
        check(near(boxMin(before), 0.0f, 0.0f, 0.0f) && near(boxMax(before), 1.0f, 1.0f, 0.0f),
            "without a morph the bounds are the rest pose: " + describe(before));

        // Attached AFTER the bounds were computed and cached: the setter must invalidate.
        instance.setMorphInstance(std::make_shared<MorphInstance>(
            std::make_shared<Morph>(twoTargets(), 3, nullptr)));
        const BoundingBox after = instance.aabb();
        check(near(boxMin(after), 0.0f, 0.0f, -2.0f) && near(boxMax(after), 4.0f, 1.0f, 1.0f),
            "with the morph they grow by its delta bounds: " + describe(after));
    }

    std::cout << "\nskinned + morphed glTF bone bounds\n";
    {
        auto device = std::make_shared<StubDevice>();
        tinygltf::Model model = skinnedMorphedModel();
        auto container = GlbParser::createFromModel(model, device, "morphBoundsTests");
        check(container != nullptr, "createFromModel parses the skinned, morphed model");
        const auto* skinContainer = container.get();
        const bool hasBoneBox = skinContainer && skinContainer->skinPayloads().size() == 1 &&
            skinContainer->skinPayloads()[0].skin && skinContainer->skinPayloads()[0].skin->hasBoneAabbs();
        check(hasBoneBox, "the skin carries bone bounds");
        if (hasBoneBox) {
            const BoundingBox& bone = skinContainer->skinPayloads()[0].skin->boneAabbs()[0];
            // Vertex 0 reaches z -2 (A) and z +1 (B); vertex 1 reaches x 1 + 3 = 4 (B).
            check(near(boxMin(bone), 0.0f, 0.0f, -2.0f) && near(boxMax(bone), 4.0f, 1.0f, 1.0f),
                "the bone box holds every vertex's reach under its targets: " + describe(bone));
        }
    }

    std::cout << (failures == 0 ? "\nAll morph bounds tests passed\n" : "\nMorph bounds tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
