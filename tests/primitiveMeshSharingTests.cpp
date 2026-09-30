// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Every render component of one primitive type shares ONE mesh per device (upstream's
// getShapePrimitive). Until 2026-09-30 each component built its own geometry and GPU
// buffers — ten thousand boxes were ten thousand box meshes — and setMaterial rebuilt
// the primitive, so the usual setType-then-setMaterial order built it twice. None of
// that shows in a frame: a shared mesh and a private copy render the same pixels. So the
// buffers are counted here, on a device whose buffers count themselves:
//
//  - components of one type share a mesh, and a different type has its own;
//  - setMaterial swaps the instance's material in place, as upstream: no rebuild, and
//    the instance keeps what was set on it;
//  - the cache holds meshes WEAKLY (a deviation): the last component to go frees the
//    mesh, and the next one builds it again;
//  - a clone shares its source's mesh.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/appOptions.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;
    int liveVertexBuffers = 0;
    int vertexBuffersCreated = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    class CountingVertexBuffer final : public VertexBuffer
    {
    public:
        CountingVertexBuffer(GraphicsDevice* device, const std::shared_ptr<VertexFormat>& format,
            const int numVertices, const VertexBufferOptions& options)
            : VertexBuffer(device, format, numVertices, options)
        {
            ++liveVertexBuffers;
            ++vertexBuffersCreated;
        }
        ~CountingVertexBuffer() override { --liveVertexBuffers; }
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
            return std::make_shared<CountingVertexBuffer>(this, format, numVertices, options);
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

    Entity* addPrimitive(Engine& engine, const std::string& type, Material* material)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        engine.root()->addChild(std::move(owned));
        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        // setType FIRST: the order that used to build the primitive twice.
        render->setType(type);
        render->setMaterial(material);
        return entity;
    }

    MeshInstance* instanceOf(Entity* entity)
    {
        auto* render = entity ? entity->findComponent<RenderComponent>() : nullptr;
        return render && !render->meshInstances().empty() ? render->meshInstances()[0] : nullptr;
    }

    const Mesh* meshOf(Entity* entity)
    {
        const MeshInstance* instance = instanceOf(entity);
        return instance ? instance->mesh() : nullptr;
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubDevice>();
    auto engine = std::make_shared<Engine>(nullptr);
    AppOptions options;
    options.graphicsDevice = device;
    options.registerComponentSystem<RenderComponentSystem>();
    engine->init(options);

    auto materialA = std::make_shared<StandardMaterial>();
    auto materialB = std::make_shared<StandardMaterial>();

    std::cout << "sharing\n";
    std::vector<Entity*> boxes;
    for (int i = 0; i < 100; ++i) {
        boxes.push_back(addPrimitive(*engine, "box", materialA.get()));
    }
    check(meshOf(boxes[0]) != nullptr, "a box component has a mesh");
    bool allShared = true;
    for (Entity* box : boxes) {
        allShared = allShared && meshOf(box) == meshOf(boxes[0]);
    }
    check(allShared, "a hundred box components share one mesh");
    check(vertexBuffersCreated == 1 && liveVertexBuffers == 1,
        "and one vertex buffer was built for them, not one per component or two");

    Entity* sphere = addPrimitive(*engine, "sphere", materialA.get());
    check(meshOf(sphere) != nullptr && meshOf(sphere) != meshOf(boxes[0]), "a sphere has a mesh of its own");
    check(liveVertexBuffers == 2, "one per primitive type in use");

    std::cout << "\nsetMaterial\n";
    {
        auto* render = boxes[1]->findComponent<RenderComponent>();
        MeshInstance* before = instanceOf(boxes[1]);
        before->setCastShadow(false);   // per-instance state a rebuild used to drop
        render->setMaterial(materialB.get());
        MeshInstance* after = instanceOf(boxes[1]);
        check(after == before, "keeps the mesh instance: nothing is rebuilt");
        check(after->material() == materialB.get(), "and gives it the new material");
        check(!after->castShadow(), "which keeps what was set on it");
        check(instanceOf(boxes[2])->material() == materialA.get(), "the other boxes keep theirs");
        check(vertexBuffersCreated == 2, "no buffer was built");
    }

    std::cout << "\nsetType\n";
    {
        auto* render = boxes[3]->findComponent<RenderComponent>();
        render->setType("sphere");
        check(meshOf(boxes[3]) == meshOf(sphere), "switching a box to a sphere shares the sphere's mesh");
        check(instanceOf(boxes[3])->material() == materialA.get(), "and keeps the component's material");
        check(vertexBuffersCreated == 2, "still no buffer was built");
    }

    std::cout << "\nclone\n";
    {
        Entity* copy = boxes[4]->clone();
        engine->root()->addChild(std::unique_ptr<Entity>(copy));
        check(meshOf(copy) == meshOf(boxes[4]), "a clone shares its source's mesh");
        check(instanceOf(copy) != instanceOf(boxes[4]), "through an instance of its own");
        boxes.push_back(copy);
    }

    std::cout << "\nlifetime\n";
    {
        // Every box goes (box 3 is a sphere now); the sphere mesh stays in use.
        for (Entity* box : boxes) {
            if (box != boxes[3]) {
                engine->root()->removeChild(box).reset();
            }
        }
        check(liveVertexBuffers == 1, "the last box going frees the box mesh; the spheres keep theirs");
        Entity* another = addPrimitive(*engine, "box", materialA.get());
        check(meshOf(another) != nullptr && liveVertexBuffers == 2 && vertexBuffersCreated == 3,
            "the next box builds the mesh again");
        engine->root()->removeChild(another).reset();
        engine->root()->removeChild(boxes[3]).reset();
        engine->root()->removeChild(sphere).reset();
        check(liveVertexBuffers == 0, "and nothing is left once no component uses a primitive");
    }

    std::cout << (failures == 0 ? "\nAll primitive mesh sharing tests passed\n"
        : "\nPrimitive mesh sharing tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
