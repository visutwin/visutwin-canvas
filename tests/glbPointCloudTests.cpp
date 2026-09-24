// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A glTF POINTS primitive is a point cloud: position plus COLOR_0, drawn unlit with
// vertex colours. Until 2026-09-24 only the synchronous parse() knew that. The two
// asynchronous paths (createFromModel, and prepareFromModel + createFromPrepared,
// which is what loadAsync uses) pushed POINTS through the triangle layout, with no
// colours and no point variant, because each path carried its own copy of the
// vertex extraction. All three now run one pipeline, and this pins what it builds:
//   - without animations, every cloud is baked into WORLD space and merged into ONE
//     draw under a synthetic root node, and a leaf node that held only points is
//     skipped;
//   - with animations, each cloud stays in its node's LOCAL space as its own
//     payload, so animating the node still moves it.
// The model is built in memory: a triangle under node "Tri", and a two-point cloud
// under node "Points", translated by (10, 0, 0).

#include <tiny_gltf.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/glbParser.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/materials/material.h"
#include "scene/mesh.h"

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

    class CpuVertexBuffer final : public VertexBuffer
    {
    public:
        using VertexBuffer::VertexBuffer;
        void unlock() override {}
    };

    class CpuIndexBuffer final : public IndexBuffer
    {
    public:
        using IndexBuffer::IndexBuffer;
        bool setData(const std::vector<uint8_t>&) override { return true; }
    };

    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            const int numVertices, const VertexBufferOptions& options) override
        {
            return std::make_shared<CpuVertexBuffer>(this, format, numVertices, options);
        }
        std::shared_ptr<IndexBuffer> createIndexBuffer(const IndexFormat format, const int numIndices,
            const std::vector<uint8_t>& data) override
        {
            auto buffer = std::make_shared<CpuIndexBuffer>(this, format, numIndices);
            buffer->setData(data);
            return buffer;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

    int addAccessor(tinygltf::Model& model, std::vector<unsigned char>& bytes, const std::vector<float>& values,
        const int count, const int type)
    {
        const size_t at = bytes.size();
        bytes.resize(at + values.size() * sizeof(float));
        std::memcpy(bytes.data() + at, values.data(), values.size() * sizeof(float));

        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = at;
        view.byteLength = values.size() * sizeof(float);
        model.bufferViews.push_back(view);

        tinygltf::Accessor accessor;
        accessor.bufferView = static_cast<int>(model.bufferViews.size()) - 1;
        accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        accessor.count = static_cast<size_t>(count);
        accessor.type = type;
        model.accessors.push_back(accessor);
        return static_cast<int>(model.accessors.size()) - 1;
    }

    tinygltf::Model buildModel(const bool animated)
    {
        tinygltf::Model model;
        model.asset.version = "2.0";
        tinygltf::Buffer buffer;

        tinygltf::Primitive triangle;
        triangle.mode = TINYGLTF_MODE_TRIANGLES;
        triangle.attributes["POSITION"] = addAccessor(model, buffer.data,
            {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f}, 3, TINYGLTF_TYPE_VEC3);
        tinygltf::Mesh triangleMesh;
        triangleMesh.primitives.push_back(triangle);
        model.meshes.push_back(triangleMesh);

        tinygltf::Primitive points;
        points.mode = TINYGLTF_MODE_POINTS;
        points.attributes["POSITION"] = addAccessor(model, buffer.data,
            {1.0f, 2.0f, 3.0f,  -1.0f, 0.0f, 0.5f}, 2, TINYGLTF_TYPE_VEC3);
        points.attributes["COLOR_0"] = addAccessor(model, buffer.data,
            {1.0f, 0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f, 0.5f}, 2, TINYGLTF_TYPE_VEC4);
        tinygltf::Mesh pointMesh;
        pointMesh.primitives.push_back(points);
        model.meshes.push_back(pointMesh);
        model.buffers.push_back(buffer);

        tinygltf::Node triNode;
        triNode.name = "Tri";
        triNode.mesh = 0;
        tinygltf::Node pointNode;
        pointNode.name = "Points";
        pointNode.mesh = 1;
        pointNode.translation = {10.0, 0.0, 0.0};
        model.nodes = {triNode, pointNode};
        tinygltf::Scene scene;
        scene.nodes = {0, 1};
        model.scenes.push_back(scene);
        model.defaultScene = 0;

        if (animated) {
            // Any animation at all switches the clouds to per-node local space.
            const int times = addAccessor(model, model.buffers[0].data, {0.0f, 1.0f}, 2, TINYGLTF_TYPE_SCALAR);
            const int values = addAccessor(model, model.buffers[0].data,
                {10.0f, 0.0f, 0.0f,  12.0f, 0.0f, 0.0f}, 2, TINYGLTF_TYPE_VEC3);
            tinygltf::AnimationSampler sampler;
            sampler.input = times;
            sampler.output = values;
            sampler.interpolation = "LINEAR";
            tinygltf::AnimationChannel channel;
            channel.sampler = 0;
            channel.target_node = 1;
            channel.target_path = "translation";
            tinygltf::Animation animation;
            animation.name = "slide";
            animation.samplers.push_back(sampler);
            animation.channels.push_back(channel);
            model.animations.push_back(animation);
        }
        return model;
    }

    // The point payload (the one drawn as POINTS), or null.
    const GlbMeshPayload* pointPayload(const GlbContainerResource& container, int& count)
    {
        const GlbMeshPayload* found = nullptr;
        count = 0;
        for (const auto& payload : container.meshPayloads()) {
            if (payload.mesh && payload.mesh->getPrimitive().type == PRIMITIVE_POINTS) {
                found = &payload;
                ++count;
            }
        }
        return found;
    }

    bool near(const float a, const float b) { return std::fabs(a - b) < 1e-5f; }

    void checkContainer(const GlbContainerResource* container, const bool animated, const std::string& path)
    {
        check(container != nullptr, path + ": builds a container");
        if (!container) {
            return;
        }
        int pointPayloads = 0;
        const GlbMeshPayload* points = pointPayload(*container, pointPayloads);
        check(pointPayloads == 1 && points, path + ": the cloud is ONE payload drawn as POINTS");
        if (!points) {
            return;
        }
        const auto vb = points->mesh->getVertexBuffer();
        check(vb && vb->format()->size() == 28, path + ": in the 28-byte position + colour layout");
        check(!points->castShadow, path + ": casting no shadow");
        const uint64_t key = points->material ? points->material->shaderVariantKey() : 0;
        check((key & (1ull << 21)) && (key & (1ull << 31)), path + ": with vertex colours and point size");
        check(((key & (1ull << 32)) != 0) == animated, path + (animated ? ": unlit (animated)" : ": lit (static)"));

        if (!vb || vb->storage().size() < 2 * 28) {
            return;
        }
        float v[14];
        std::memcpy(v, vb->storage().data(), sizeof(v));
        // First point (1, 2, 3) in red; second (-1, 0, 0.5) in blue at half alpha.
        const float x0 = animated ? 1.0f : 11.0f;
        check(near(v[0], x0) && near(v[1], 2.0f) && near(v[2], 3.0f),
            path + (animated ? ": positions stay in the node's local space"
                             : ": positions are baked into world space (node at x = 10)"));
        check(near(v[3], 1.0f) && near(v[5], 0.0f) && near(v[12], 1.0f) && near(v[13], 0.5f),
            path + ": COLOR_0 reaches the vertices");

        const auto& nodes = container->nodePayloads();
        const auto& roots = container->rootNodeIndices();
        if (animated) {
            check(nodes.size() == 2 && !nodes[1].skip && nodes[1].meshPayloadIndices.size() == 1,
                path + ": the cloud stays on its own node, which is kept");
            check(roots.size() == 2, path + ": no synthetic root");
        } else {
            check(nodes.size() == 3 && nodes[2].name == "__merged_point_cloud",
                path + ": the merged cloud hangs off a synthetic node");
            check(nodes.size() >= 2 && nodes[1].skip, path + ": the points-only leaf node is skipped");
            check(!roots.empty() && roots.front() == 2, path + ": the synthetic node is a root");
        }
    }
}

int main()
{
    auto device = std::make_shared<StubDevice>();

    for (const bool animated : {false, true}) {
        const std::string label = animated ? "animated" : "static";
        std::cout << (animated ? "\n" : "") << label << " model\n";
        {
            tinygltf::Model model = buildModel(animated);
            const auto container = GlbParser::createFromModel(model, device, "glbPointCloudTests");
            checkContainer(container.get(), animated, "createFromModel (" + label + ")");
        }
        {
            tinygltf::Model model = buildModel(animated);
            auto prepared = GlbParser::prepareFromModel(model, PixelFormat::PIXELFORMAT_RGBA8, "glbPointCloudTests");
            const auto container = GlbParser::createFromPrepared(model, std::move(prepared), device,
                "glbPointCloudTests");
            checkContainer(container.get(), animated, "createFromPrepared (" + label + ")");
        }
    }

    std::cout << (failures == 0 ? "\nAll GLB point cloud tests passed\n" : "\nGLB point cloud tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
