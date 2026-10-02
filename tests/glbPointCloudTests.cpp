// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.09.2026
//
// A glTF POINTS primitive is a point cloud: position plus COLOR_0, drawn unlit with
// vertex colours, on every load path: the synchronous parse() and the two
// asynchronous ones (createFromModel, and prepareFromModel + createFromPrepared,
// which is what loadAsync uses). A path with its own copy of the vertex extraction
// pushes POINTS through the triangle layout, with no colours and no point variant.
// All three run one pipeline, and this pins what it builds:
//   - without animations, every cloud is baked into WORLD space and merged into ONE
//     draw under a synthetic root node, and a leaf node that held only points is
//     skipped;
//   - with animations, each cloud stays in its node's LOCAL space as its own
//     payload, so animating the node still moves it.
// The model is built in memory: a triangle under node "Tri", and a two-point cloud
// under node "Points", translated by (10, 0, 0).

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/parsers/glbContainerResource.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/materials/material.h"
#include "scene/mesh.h"
#include "support/check.h"
#include "support/gltfModel.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kTolerance = 1e-5f;

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
        check(nearStrict(v[0], x0, kTolerance) && nearStrict(v[1], 2.0f, kTolerance) &&
              nearStrict(v[2], 3.0f, kTolerance),
            path + (animated ? ": positions stay in the node's local space"
                             : ": positions are baked into world space (node at x = 10)"));
        check(nearStrict(v[3], 1.0f, kTolerance) && nearStrict(v[5], 0.0f, kTolerance) &&
              nearStrict(v[12], 1.0f, kTolerance) && nearStrict(v[13], 0.5f, kTolerance),
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
    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.cpuBuffers = true});

    for (const bool animated : {false, true}) {
        const std::string label = animated ? "animated" : "static";
        std::cout << (animated ? "\n" : "") << label << " model\n";
        forEachLoadPath([animated] { return buildModel(animated); }, device, "glbPointCloudTests",
            [&](const GlbContainerResource* container, const LoadPath path) {
                const std::string entry =
                    path == LoadPath::CreateFromModel ? "createFromModel (" : "createFromPrepared (";
                checkContainer(container, animated, entry + label + ")");
            });
    }

    return finish("GLB point cloud");
}
