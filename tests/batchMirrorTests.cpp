// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// A batch draws with an identity node, so the culling flip a mirrored node gets when it
// is drawn on its own (front and back swapped for a negative determinant) does not reach
// its triangles once they are merged. Unless the merge re-winds them, a box scaled
// (-1, 1, 1) in a batch group shows its inside: every one of its triangles winds
// clockwise seen from outside, and back-face culling removes the faces that should show.
//
// The oracle is the geometry itself: for every merged triangle, the normal its winding
// gives (counter-clockwise front faces) must agree with the vertex normals it carries.
// Checked for a static batch (positions merged in world space) and a dynamic one
// (positions in each source's local space, placed by its node).

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "framework/batching/batch.h"
#include "framework/batching/batchGroup.h"
#include "framework/batching/batchManager.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr int kStaticGroup = 1;
    constexpr int kDynamicGroup = 2;

    Vector3 readVector3(const std::vector<uint8_t>& bytes, const size_t offset)
    {
        float xyz[3];
        std::memcpy(xyz, bytes.data() + offset, sizeof(xyz));
        return Vector3(xyz[0], xyz[1], xyz[2]);
    }

    struct Facing
    {
        int triangles = 0;
        int agreeing = 0;
    };

    // Counts the batch's triangles whose winding agrees with their vertex normals, in
    // world space. A dynamic batch's vertex carries its source's slot after the packed
    // 56 bytes, and that source's node places it.
    Facing countFacing(const Batch& batch)
    {
        Facing facing;
        const auto& vertices = batch.vertexBuffer->storage();
        const auto& indexBytes = batch.indexBuffer->storage();
        const size_t stride = static_cast<size_t>(batch.vertexBuffer->format()->size());
        const size_t indexCount = indexBytes.size() / sizeof(uint32_t);

        const auto worldVertex = [&](const uint32_t index, Vector3& position, Vector3& normal) {
            const size_t base = static_cast<size_t>(index) * stride;
            position = readVector3(vertices, base);
            normal = readVector3(vertices, base + 12);
            if (batch.dynamic) {
                float slot = 0.0f;
                std::memcpy(&slot, vertices.data() + base + 56, sizeof(slot));
                const MeshInstance* source = batch.origMeshInstances[static_cast<size_t>(slot)];
                const Matrix4 world = source->node()->worldTransform();
                position = world.transformPoint(position);
                normal = normal.transformNormal(world.inverse().transpose());
            }
        };

        for (size_t i = 0; i + 2 < indexCount; i += 3) {
            uint32_t corner[3];
            std::memcpy(corner, indexBytes.data() + i * sizeof(uint32_t), sizeof(corner));
            Vector3 p[3];
            Vector3 n[3];
            for (int c = 0; c < 3; ++c) {
                worldVertex(corner[c], p[c], n[c]);
            }
            const Vector3 wound = (p[1] - p[0]).cross(p[2] - p[0]);
            ++facing.triangles;
            if (wound.dot(n[0] + n[1] + n[2]) > 0.0f) {
                ++facing.agreeing;
            }
        }
        return facing;
    }

    const Batch* batchOf(BatchManager& batcher, const int groupId)
    {
        for (const auto& batch : batcher.batches()) {
            if (batch && batch->batchGroupId == groupId) {
                return batch.get();
            }
        }
        return nullptr;
    }
}

int main()
{
    std::cout << std::unitbuf;

    // The merged index buffer has to keep its bytes for the check to read them.
    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
        .cpuBuffers = true, .keepIndexData = true});
    auto engine = makeTestEngine<RenderComponentSystem>(device);
    BatchManager& batcher = *engine->batcher();
    batcher.addGroup(BatchGroup(kStaticGroup, "static", /*dynamic*/ false));
    batcher.addGroup(BatchGroup(kDynamicGroup, "dynamic", /*dynamic*/ true));

    auto material = std::make_shared<StandardMaterial>();
    for (const int groupId : {kStaticGroup, kDynamicGroup}) {
        // One plain box and one mirrored across X, side by side.
        for (int i = 0; i < 2; ++i) {
            auto owned = std::make_unique<Entity>();
            Entity* box = owned.get();
            box->setEngine(engine.get());
            box->setName("Box" + std::to_string(groupId) + std::to_string(i));
            engine->root()->addChild(std::move(owned));
            box->setLocalPosition(static_cast<float>(i) * 3.0f, static_cast<float>(groupId) * 3.0f, 0.0f);
            if (i == 1) {
                box->setLocalScale(-1.0f, 1.0f, 1.0f);
            }
            auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
            render->setMaterial(material.get());
            render->setType("box");
            render->setBatchGroupId(groupId);
        }
    }
    batcher.prepare(engine->scene().get());

    for (const int groupId : {kStaticGroup, kDynamicGroup}) {
        const std::string kind = groupId == kStaticGroup ? "static" : "dynamic";
        const Batch* batch = batchOf(batcher, groupId);
        check(batch && batch->origMeshInstances.size() == 2,
            "the " + kind + " group merges the plain and the mirrored box into one batch");
        if (!batch || !batch->vertexBuffer || !batch->indexBuffer) {
            continue;
        }
        int sourceTriangles = 0;
        for (const MeshInstance* source : batch->origMeshInstances) {
            const auto indexBuffer = source->mesh()->getIndexBuffer();
            sourceTriangles += indexBuffer ? indexBuffer->numIndices() / 3 : 0;
        }
        const Facing facing = countFacing(*batch);
        check(facing.triangles > 0 && facing.triangles == sourceTriangles,
            "the " + kind + " batch holds both boxes' triangles (" + std::to_string(facing.triangles) +
            " of " + std::to_string(sourceTriangles) + ")");
        check(facing.agreeing == facing.triangles,
            "every " + kind + " triangle winds counter-clockwise around its normal, the mirrored box's too (" +
            std::to_string(facing.agreeing) + " of " + std::to_string(facing.triangles) + ")");
    }

    return finish("batch mirror");
}
