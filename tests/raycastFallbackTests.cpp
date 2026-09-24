// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// With no PhysicsWorld supplied, RigidBodyComponentSystem::raycastFirst/raycastAll
// sweep the collision bounds on the CPU. That sweep tested the components' own
// enabled() flags and never the entity, so a collider on a DISABLED entity — or under
// a disabled parent — was still hit (fixed 2026-09-24). It must answer the same
// question every gathering loop does: Component::active().

#include <iostream>
#include <memory>

#include "framework/appOptions.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"

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

    // A unit box collider with a static body, at `z` on the ray's path.
    Entity* addBox(Engine& engine, GraphNode* parent, const float z)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        parent->addChild(std::move(owned));
        entity->setLocalPosition(0.0f, 0.0f, z);
        auto* collision = static_cast<CollisionComponent*>(entity->addComponent<CollisionComponent>());
        collision->setType("box");
        collision->setHalfExtents(Vector3(0.5f, 0.5f, 0.5f));
        entity->addComponent<RigidBodyComponent>();
        return entity;
    }
}

int main()
{
    auto engine = std::make_shared<Engine>(nullptr);
    AppOptions options;
    options.graphicsDevice = std::make_shared<StubDevice>();
    options.registerComponentSystem<CollisionComponentSystem>();
    options.registerComponentSystem<RigidBodyComponentSystem>();
    engine->init(options);   // no physicsWorld: the CPU fallback answers

    auto* system = dynamic_cast<RigidBodyComponentSystem*>(engine->systems()->getById("rigidbody"));
    check(system != nullptr, "the rigid-body system exists");
    if (!system) {
        return 1;
    }

    // Two boxes along -Z; the ray starts in front of both.
    Entity* nearBox = addBox(*engine, engine->root(), 0.0f);
    auto parentOwned = std::make_unique<Entity>();
    Entity* parent = parentOwned.get();
    parent->setEngine(engine.get());
    engine->root()->addChild(std::move(parentOwned));
    Entity* farBox = addBox(*engine, parent, -5.0f);

    const Vector3 start(0.0f, 0.0f, 10.0f);
    const Vector3 end(0.0f, 0.0f, -10.0f);

    std::cout << "raycast CPU fallback\n";
    check(system->raycastAll(start, end).size() == 2, "both boxes are hit while enabled");
    const auto first = system->raycastFirst(start, end);
    check(first && first->entity == nearBox, "raycastFirst hits the nearer box");

    nearBox->setEnabled(false);
    const auto afterDisable = system->raycastFirst(start, end);
    check(afterDisable && afterDisable->entity == farBox, "a DISABLED entity's collider is not hit");

    parent->setEnabled(false);
    check(system->raycastAll(start, end).empty(), "nor one under a disabled PARENT");

    std::cout << (failures == 0 ? "\nAll raycast fallback tests passed\n" : "\nRaycast fallback tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
