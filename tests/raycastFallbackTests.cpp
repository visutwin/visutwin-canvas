// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.09.2026
//
// With no PhysicsWorld supplied, RigidBodyComponentSystem::raycastFirst/raycastAll
// sweep the collision bounds on the CPU. A sweep that tests the components' own
// enabled() flags and never the entity still hits a collider on a DISABLED entity —
// or under a disabled parent. It must answer the same question every gathering loop
// does: Component::active().

#include <iostream>
#include <memory>

#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
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
    // No physicsWorld: the CPU fallback answers.
    auto engine = makeTestEngine<CollisionComponentSystem, RigidBodyComponentSystem>(
        std::make_shared<StubGraphicsDevice>());

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

    return finish("raycast fallback");
}
