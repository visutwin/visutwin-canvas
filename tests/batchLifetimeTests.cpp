// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A batch keeps raw pointers to its SOURCE mesh instances (and a dynamic batch to
// their nodes, read every frame). Until 2026-09-23 nothing told the BatchManager
// when a source went away, so destroying a batched entity left a batch pointing
// at freed mesh instances until the group happened to be rebuilt or destroyed —
// and for a dynamic batch the very next frame read them. Disabling an entity did
// not take its meshes out of a rebuild either: the rebuild tested enabled(), not
// active().
//
// A use-after-free cannot be observed in a result, so this pins the contract that
// prevents it: the moment a source leaves (destroyed, disabled, moved to another
// group), no batch of its group points at it, and the next update rebuilds the
// group from what remains.

#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

#include "framework/appOptions.h"
#include "framework/batching/batchGroup.h"
#include "framework/batching/batchManager.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    // Buffers that keep their bytes on the CPU, which is all batching reads.
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

    constexpr int kGroup = 3;

    std::vector<MeshInstance*> sourcesInGroup(BatchManager& batcher, const int groupId)
    {
        std::vector<MeshInstance*> sources;
        for (const auto& batch : batcher.batches()) {
            if (batch && batch->batchGroupId == groupId) {
                sources.insert(sources.end(), batch->origMeshInstances.begin(), batch->origMeshInstances.end());
            }
        }
        return sources;
    }

    bool contains(const std::vector<MeshInstance*>& list, const MeshInstance* item)
    {
        return std::find(list.begin(), list.end(), item) != list.end();
    }
}

int main()
{
    auto device = std::make_shared<StubDevice>();
    auto engine = std::make_shared<Engine>(nullptr);
    AppOptions options;
    options.graphicsDevice = device;
    options.registerComponentSystem<RenderComponentSystem>();
    engine->init(options);
    BatchManager& batcher = *engine->batcher();

    // One DYNAMIC group: its batch reads its sources' nodes every frame, the case
    // where a dangling source is read soonest.
    batcher.addGroup(BatchGroup(kGroup, "boxes", /*dynamic*/ true));

    auto material = std::make_shared<StandardMaterial>();
    std::vector<Entity*> boxes;
    std::vector<MeshInstance*> meshes;
    for (int i = 0; i < 4; ++i) {
        auto owned = std::make_unique<Entity>();
        Entity* box = owned.get();
        box->setEngine(engine.get());
        engine->root()->addChild(std::move(owned));
        box->setLocalPosition(static_cast<float>(i) * 2.0f, 0.0f, 0.0f);
        auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
        render->setMaterial(material.get());
        render->setType("box");
        render->setBatchGroupId(kGroup);
        boxes.push_back(box);
        meshes.push_back(render->meshInstances().empty() ? nullptr : render->meshInstances()[0]);
    }
    batcher.prepare(engine->scene().get());
    check(sourcesInGroup(batcher, kGroup).size() == 4, "prepare merges the four boxes into the group's batch");

    std::cout << "\ndestroying a batched entity\n";
    MeshInstance* destroyedMesh = meshes[0];
    boxes[0]->destroy();
    check(!contains(sourcesInGroup(batcher, kGroup), destroyedMesh),
        "the moment it is destroyed, no batch points at its mesh instance");
    engine->root()->removeChild(boxes[0]).reset(); // frees the entity and its mesh instance
    batcher.updateAll();                         // a dynamic batch reads its sources here
    const auto afterDestroy = sourcesInGroup(batcher, kGroup);
    check(afterDestroy.size() == 3, "the next update rebuilds the group from the three that remain");

    std::cout << "\ndisabling a batched entity\n";
    boxes[1]->setEnabled(false);
    check(!contains(sourcesInGroup(batcher, kGroup), meshes[1]),
        "disabling it takes it out of every batch at once");
    batcher.updateAll();
    const auto afterDisable = sourcesInGroup(batcher, kGroup);
    check(afterDisable.size() == 2 && !contains(afterDisable, meshes[1]),
        "and the rebuild leaves it out: a disabled entity is not active()");
    boxes[1]->setEnabled(true);
    batcher.updateAll();
    check(sourcesInGroup(batcher, kGroup).size() == 3, "re-enabling it brings it back at the next update");

    std::cout << "\nmoving a source to another group\n";
    boxes[2]->findComponent<RenderComponent>()->setBatchGroupId(BatchGroup::NOID);
    check(!contains(sourcesInGroup(batcher, kGroup), meshes[2]), "leaving the group takes it out at once");
    batcher.updateAll();
    check(sourcesInGroup(batcher, kGroup).size() == 2, "and the group is rebuilt without it");

    std::cout << (failures == 0 ? "\nAll batch lifetime tests passed\n" : "\nBatch lifetime tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
