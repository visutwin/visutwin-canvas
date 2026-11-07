// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
// A (camera, layer) cull set is kept across frames and reused while nothing it reads
// changed (cullVersion.h): a static scene skips the sweep and the sort. Every input
// the sweep or the sort reads is moved here in turn — a transform, a material, a mask,
// visibility, layers, an entity's and a parent's enabled state, an instance added and
// one destroyed, a batch, a mesh's bounds, a material's alpha mode, the camera's pose
// and mask — and each must re-cull the frame it moves and reuse again the frame after.
// The draw counts say the re-cull saw the change, and the material switch count says
// the kept draw ORDER is the sorted one (the boxes are created A, B, A and draw A, A, B).
// VISUTWIN_CULL_CHECK is on for the whole run, so every reused set and order is also
// held against a fresh sweep and sort inside the renderer.

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/applicationStats.h"
#include "framework/batching/batchGroup.h"
#include "framework/batching/batchManager.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/cullVersion.h"
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
    Entity* addEntity(Engine& engine, GraphNode* parent, const std::string& name)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        entity->setName(name);
        parent->addChild(std::move(owned));
        return entity;
    }

    RenderComponent* addBox(Engine& engine, GraphNode* parent, const std::string& name, Material* material, float x)
    {
        Entity* box = addEntity(engine, parent, name);
        box->setLocalPosition(x, 0.0f, 0.0f);
        auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
        render->setMaterial(material);
        render->setType("box");
        return render;
    }

    struct Frame
    {
        int forward = 0;
        int materials = 0;
        int reused = 0;
    };

    Frame readFrame(Engine& engine)
    {
        auto& stats = const_cast<ApplicationStats&>(*engine.stats());
        return {stats.drawCalls().forward, stats.frame().materials, stats.frame().cullSetsReused};
    }

    Frame runFrame(Engine& engine)
    {
        engine.update(1.0f / 60.0f);
        engine.render();
        return readFrame(engine);
    }
}

int main()
{
    std::cout << std::unitbuf;
    // Every reused set and draw order is compared with a fresh sweep and sort.
    setenv("VISUTWIN_CULL_CHECK", "1", 1);

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
        .size = {64, 64}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(device);

    Entity* cameraEntity = addEntity(*engine, engine->root(), "Camera");
    cameraEntity->setLocalPosition(0.0f, 0.0f, 12.0f);
    auto* cameraComponent = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());

    auto materialA = std::make_shared<StandardMaterial>();
    auto materialB = std::make_shared<StandardMaterial>();
    // A, B, A in creation order: sorted by material they draw A, A, B, two switches.
    Entity* group = addEntity(*engine, engine->root(), "Group");
    RenderComponent* box0 = addBox(*engine, engine->root(), "Box0", materialA.get(), -2.5f);
    RenderComponent* box1 = addBox(*engine, engine->root(), "Box1", materialB.get(), 0.0f);
    RenderComponent* box2 = addBox(*engine, group, "Box2", materialA.get(), 2.5f);
    std::cout << "a static scene\n";
    engine->start();
    // start() ticked once; a bump makes the next frame the first to sweep this scene.
    CullVersion::bump();
    const Frame first = runFrame(*engine);
    check(first.reused == 0 && first.forward == 3 && first.materials == 2,
        "the first frame sweeps: 3 draws, 2 material switches (A, A, B)");
    const Frame second = runFrame(*engine);
    check(second.reused > 0, "the second frame reuses every (camera, layer) set: " + std::to_string(second.reused));
    check(second.forward == 3 && second.materials == 2,
        "and draws the same 3 draws in the kept sorted order (2 switches, not the 3 of A, B, A)");
    const int steadyReuse = second.reused;
    check(runFrame(*engine).reused == steadyReuse, "a third frame reuses the same sets");

    // Moves one input, checks the next frame re-culls (nothing reused) and draws
    // `forward` draws, then that the frame after reuses again.
    const auto moved = [&](const std::string& what, const int forward) {
        const Frame changed = runFrame(*engine);
        check(changed.reused == 0 && changed.forward == forward,
            what + ": re-culled (reused " + std::to_string(changed.reused) + "), " + std::to_string(changed.forward) +
            " draws");
        const Frame after = runFrame(*engine);
        check(after.reused == steadyReuse && after.forward == forward,
            what + ": reused again the frame after (" + std::to_string(after.reused) + ")");
    };

    std::cout << "\neach input the sweep or the sort reads\n";
    box0->entity()->setLocalPosition(-3.0f, 0.5f, 0.0f);
    moved("a box moved", 3);
    group->setLocalPosition(0.0f, 0.0f, -1.0f);
    moved("a box's parent moved", 3);
    box1->setMaterial(materialA.get());
    moved("a material swapped", 3);
    box1->setMaterial(materialB.get());
    moved("and swapped back", 3);
    materialA->setAlphaMode(AlphaMode::BLEND);
    moved("a material made transparent", 3);
    materialA->setAlphaMode(AlphaMode::OPAQUE);
    moved("and opaque again", 3);

    MeshInstance* mesh1 = box1->meshInstances().at(0);
    mesh1->setMask(0u);
    moved("an instance mask that the camera rejects", 2);
    mesh1->setMask(MASK_AFFECT_DYNAMIC);
    moved("and accepts again", 3);
    mesh1->setVisible(false);
    moved("an instance hidden", 2);
    mesh1->setVisible(true);
    moved("and shown", 3);
    mesh1->mesh()->setAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.75f, 0.75f, 0.75f)));
    moved("a mesh's bounds", 3);

    // A write of the value already held is not a change: the HUD and the immediate
    // lines write theirs every frame, and a scene with a HUD must still reuse.
    mesh1->mesh()->setAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.75f, 0.75f, 0.75f)));
    mesh1->setVisible(true);
    mesh1->setMask(MASK_AFFECT_DYNAMIC);
    box1->setMaterial(materialB.get());
    box1->setLayers({LAYERID_WORLD});
    materialA->setTransparent(false);
    check(runFrame(*engine).reused == steadyReuse, "unchanged writes of the bounds, visibility, mask, material, layers "
        "and transparency keep the sets reused");

    box1->setLayers({LAYERID_UI});
    moved("a component moved to another layer", 2);
    box1->setLayers({LAYERID_WORLD});
    moved("and back", 3);
    box1->entity()->setEnabled(false);
    moved("an entity disabled", 2);
    box1->entity()->setEnabled(true);
    moved("and enabled", 3);
    group->setEnabled(false);
    moved("a parent disabled", 2);
    group->setEnabled(true);
    moved("and enabled", 3);

    RenderComponent* box3 = addBox(*engine, engine->root(), "Box3", materialB.get(), 5.0f);
    moved("an instance added", 4);
    Entity* box3Entity = box3->entity();
    box3Entity->destroy();
    engine->root()->removeChild(box3Entity).reset();
    moved("and destroyed", 3);

    std::cout << "\nthe camera\n";
    cameraEntity->setLocalPosition(0.0f, 1.0f, 12.0f);
    moved("the camera moved", 3);
    cameraComponent->camera()->setCullingMask(0u);
    moved("the camera's culling mask rejects everything", 0);
    cameraComponent->camera()->setCullingMask(0xFFFFFFFFu);
    moved("and accepts again", 3);

    std::cout << "\na batch\n";
    constexpr int kGroup = 7;
    engine->batcher()->addGroup(BatchGroup(kGroup, "boxes"));
    box0->setBatchGroupId(kGroup);
    box2->setBatchGroupId(kGroup);
    engine->batcher()->prepare(engine->scene().get());
    // The two sources are hidden and the batch is the layer's own instance: 2 draws.
    moved("two boxes batched", 2);
    box0->setBatchGroupId(-1);
    engine->batcher()->updateAll();
    moved("one source leaving its group", 3);

    std::cout << "\nthe in-process check\n";
    check(engine->renderer()->cullCheckMismatches() == 0,
        "VISUTWIN_CULL_CHECK found no reused set or order that a fresh sweep or sort disagrees with");

    return finish("cull-set-reuse");
}
