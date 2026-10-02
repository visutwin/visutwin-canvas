// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// Every render component of one primitive type shares ONE mesh per device. A component that built its own geometry and GPU buffers would give
// ten thousand boxes ten thousand box meshes, and a setMaterial that rebuilt the
// primitive would build it twice in the usual setType-then-setMaterial order. None of
// that shows in a frame: a shared mesh and a private copy render the same pixels. So the
// buffers are counted here, on a device whose buffers count themselves:
//
//  - components of one type share a mesh, and a different type has its own;
//  - setMaterial swaps the instance's material in place: no rebuild, and
//    the instance keeps what was set on it;
//  - the cache holds meshes WEAKLY (a deviation): the last component to go frees the
//    mesh, and the next one builds it again;
//  - a clone shares its source's mesh.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    int liveVertexBuffers = 0;
    int vertexBuffersCreated = 0;

    class CountingVertexBuffer final : public CpuVertexBuffer
    {
    public:
        CountingVertexBuffer(GraphicsDevice* device, const std::shared_ptr<VertexFormat>& format,
            const int numVertices, const VertexBufferOptions& options)
            : CpuVertexBuffer(device, format, numVertices, options)
        {
            ++liveVertexBuffers;
            ++vertexBuffersCreated;
        }
        ~CountingVertexBuffer() override { --liveVertexBuffers; }
    };

    /// CPU index buffers; every vertex buffer is counted.
    class CountingDevice final : public StubGraphicsDevice
    {
    public:
        CountingDevice() : StubGraphicsDevice(Options{.cpuBuffers = true}) {}

        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            const int numVertices, const VertexBufferOptions& options) override
        {
            return std::make_shared<CountingVertexBuffer>(this, format, numVertices, options);
        }
    };

    Entity* addPrimitive(Engine& engine, const std::string& type, Material* material)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        engine.root()->addChild(std::move(owned));
        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        // setType FIRST: the order in which a rebuilding setMaterial builds the primitive twice.
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

    auto device = std::make_shared<CountingDevice>();
    auto engine = makeTestEngine<RenderComponentSystem>(device);

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
        before->setCastShadow(false);   // per-instance state a rebuild would drop
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

    return finish("primitive mesh sharing");
}
