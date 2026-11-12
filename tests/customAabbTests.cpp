// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.11.2026
//
// A bounding box in the node's space can replace the bounds a mesh instance derives
// (MeshInstance::setLocalCustomAabb, RenderComponent::setCustomAabb). Nothing on screen tells a
// custom box from the mesh's own while both contain what is drawn, so the numbers are checked:
//
//  - the box replaces the mesh's bounds and is carried to the world through the node;
//  - it follows the node when the node moves;
//  - cleared, the instance derives its bounds from the mesh again;
//  - a world box (setCustomAabb) still wins over it;
//  - the render component gives it to the mesh instances it has, to one built later (a new
//    type), and a clone copies it.

#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "core/shape/boundingBox.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    bool sameBox(const BoundingBox& box, const Vector3& center, const Vector3& halfExtents)
    {
        constexpr float eps = 1e-5f;
        const Vector3 c = box.center();
        const Vector3 h = box.halfExtents();
        return near(c.getX(), center.getX(), eps) && near(c.getY(), center.getY(), eps) &&
            near(c.getZ(), center.getZ(), eps) && near(h.getX(), halfExtents.getX(), eps) &&
            near(h.getY(), halfExtents.getY(), eps) && near(h.getZ(), halfExtents.getZ(), eps);
    }

    MeshInstance* instanceOf(Entity* entity)
    {
        auto* render = entity ? entity->findComponent<RenderComponent>() : nullptr;
        return render && !render->meshInstances().empty() ? render->meshInstances()[0] : nullptr;
    }
}

int main()
{
    std::cout << std::unitbuf;
    quietPasses();

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.cpuBuffers = true});
    auto engine = makeTestEngine<RenderComponentSystem>(device);
    auto material = std::make_shared<StandardMaterial>();

    auto owned = std::make_unique<Entity>();
    Entity* entity = owned.get();
    entity->setEngine(engine.get());
    engine->root()->addChild(std::move(owned));
    entity->setLocalPosition(10.0f, 0.0f, 0.0f);
    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    render->setType("box");
    render->setMaterial(material.get());
    MeshInstance* box = instanceOf(entity);
    check(box != nullptr, "a box has a mesh instance");
    if (!box) {
        return finish("custom AABB");
    }

    // The unit box at x = 10.
    check(sameBox(box->aabb(), Vector3(10.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f)),
        "without a custom box the bounds are the mesh's, at the node");

    const BoundingBox custom(Vector3(0.0f, 1.0f, 0.0f), Vector3(2.0f, 3.0f, 4.0f));
    box->setLocalCustomAabb(&custom);
    check(box->localCustomAabb() != nullptr, "the instance reports its custom box");
    check(sameBox(box->aabb(), Vector3(10.0f, 1.0f, 0.0f), Vector3(2.0f, 3.0f, 4.0f)),
        "a custom box replaces the mesh's bounds, carried to the world by the node");

    entity->setLocalPosition(0.0f, 5.0f, 0.0f);
    check(sameBox(box->aabb(), Vector3(0.0f, 6.0f, 0.0f), Vector3(2.0f, 3.0f, 4.0f)),
        "and follows the node when it moves");

    entity->setLocalScale(2.0f, 2.0f, 2.0f);
    check(sameBox(box->aabb(), Vector3(0.0f, 7.0f, 0.0f), Vector3(4.0f, 6.0f, 8.0f)),
        "and its scale");
    entity->setLocalScale(1.0f, 1.0f, 1.0f);

    box->setLocalCustomAabb(nullptr);
    check(box->localCustomAabb() == nullptr, "cleared, the instance has no custom box");
    check(sameBox(box->aabb(), Vector3(0.0f, 5.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f)),
        "and derives its bounds from the mesh again");

    std::cout << "\nrender component\n";
    render->setCustomAabb(custom);
    check(render->customAabb().has_value(), "the component keeps the box");
    check(sameBox(box->aabb(), Vector3(0.0f, 6.0f, 0.0f), Vector3(2.0f, 3.0f, 4.0f)),
        "and gives it to the mesh instance it has");

    render->setType("sphere");
    MeshInstance* sphere = instanceOf(entity);
    check(sphere != nullptr && sphere->localCustomAabb() != nullptr,
        "a mesh instance built later (a new type) takes it too");
    if (sphere) {
        check(sameBox(sphere->aabb(), Vector3(0.0f, 6.0f, 0.0f), Vector3(2.0f, 3.0f, 4.0f)),
            "with the same bounds");
    }

    Entity* copy = entity->clone();
    engine->root()->addChild(copy);
    auto* copyRender = copy->findComponent<RenderComponent>();
    MeshInstance* copyInstance = instanceOf(copy);
    check(copyRender && copyRender->customAabb().has_value(), "a clone copies the component's box");
    check(copyInstance && copyInstance->localCustomAabb() != nullptr, "and its mesh instance has it");

    render->setCustomAabb(std::nullopt);
    sphere = instanceOf(entity);
    check(sphere && sphere->localCustomAabb() == nullptr, "cleared on the component, cleared on the instances");
    check(copyInstance && copyInstance->localCustomAabb() != nullptr, "and the clone keeps its own");

    std::cout << "\nworld box\n";
    if (sphere) {
        sphere->setLocalCustomAabb(&custom);
        const BoundingBox world(Vector3(100.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
        sphere->setCustomAabb(world);
        check(sameBox(sphere->aabb(), Vector3(100.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f)),
            "a world box given to setCustomAabb wins over the local one");
    }

    return finish("custom AABB");
}
