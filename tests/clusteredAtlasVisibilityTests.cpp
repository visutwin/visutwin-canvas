// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// The clustered shadow atlas is split by the number of lights it holds, so WHICH lights
// it holds decides every slot's size. Only a light some camera reaches this frame takes
// a slot: counted with the ones out of view, eight shadowed spots with two in front of
// the camera split the atlas 3x3 instead of 2x2, and each visible shadow renders at two
// thirds of the resolution it could have, for shadows nothing samples.
//
// Two things follow and are held here as well: a light culled this frame holds no slot
// (last frame's rect may belong to another light by now), and a one-shot shadow whose
// slot changes because the visible set changed is rendered again into its new slot.
//
// Rendered by the real engine on a stub device, under the default clustered lighting.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/applicationStats.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/light.h"
#include "scene/materials/standardMaterial.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    Entity* addEntity(Engine& engine, const std::string& name)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        entity->setName(name);
        engine.root()->addChild(std::move(owned));
        return entity;
    }

    bool slotIs(const Light* light, const float size)
    {
        return light->atlasViewportAllocated() && std::fabs(light->atlasViewport().getZ() - size) < 1e-5f;
    }

    // In front of the camera (it looks down -Z from the origin) or far behind it.
    constexpr float kInFront = -10.0f;
    constexpr float kBehind = 60.0f;
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
        .size = {64, 64}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, LightComponentSystem, CameraComponentSystem>(device);
    check(engine->scene()->clusteredLightingEnabled(), "clustered lighting is the default");

    Entity* cameraEntity = addEntity(*engine, "Camera");
    cameraEntity->setLocalPosition(0.0f, 0.0f, 0.0f);
    cameraEntity->addComponent<CameraComponent>();

    // Eight shadowed spots shining down: two in front of the camera, six behind it.
    std::vector<Entity*> entities;
    std::vector<LightComponent*> spots;
    for (int i = 0; i < 8; ++i) {
        Entity* entity = addEntity(*engine, "Spot" + std::to_string(i));
        const bool inFront = i < 2;
        // The two in front sit either side of the view axis, well inside the frustum.
        const float x = inFront ? (i == 0 ? -1.5f : 1.5f) : static_cast<float>(i) * 3.0f - 10.0f;
        entity->setLocalPosition(x, 2.0f, inFront ? kInFront : kBehind);
        auto* spot = static_cast<LightComponent*>(entity->addComponent<LightComponent>());
        spot->setType(LightType::LIGHTTYPE_SPOT);
        spot->setOuterConeAngle(30.0f);
        spot->setInnerConeAngle(20.0f);
        spot->setRange(4.0f);
        spot->setCastShadows(true);
        entities.push_back(entity);
        spots.push_back(spot);
    }

    // A box under the first spot, so its shadow pass has something to draw.
    auto material = std::make_shared<StandardMaterial>();
    Entity* box = addEntity(*engine, "Box");
    box->setLocalPosition(-1.5f, 0.0f, kInFront);
    auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
    render->setMaterial(material.get());
    render->setType("box");

    engine->start();
    auto& stats = const_cast<ApplicationStats&>(*engine->stats());
    const auto renderFrame = [&] {
        engine->update(1.0f / 60.0f);
        engine->render();
    };
    renderFrame();
    renderFrame();

    std::vector<Light*> lights;
    for (auto* spot : spots) {
        lights.push_back(spot->light());
    }

    // ── Only the visible lights split the atlas ──────────────────────────────────
    check(lights[0]->visibleThisFrame() && lights[1]->visibleThisFrame(), "the two spots in front are visible");
    bool behindCulled = true;
    bool behindUnallocated = true;
    for (size_t i = 2; i < lights.size(); ++i) {
        behindCulled = behindCulled && !lights[i]->visibleThisFrame();
        behindUnallocated = behindUnallocated && !lights[i]->atlasViewportAllocated();
    }
    check(behindCulled, "the six behind the camera are culled");
    check(behindUnallocated, "and hold no atlas slot");
    check(slotIs(lights[0], 0.5f) && slotIs(lights[1], 0.5f),
        "two visible lights split the atlas 2x2, not 3x3 for all eight (slot " +
        std::to_string(lights[0]->atlasViewport().getZ()) + ")");

    // ── A one-shot shadow follows its slot when the visible set changes ──────────
    spots[0]->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_THISFRAME);
    renderFrame();
    check(lights[0]->shadowUpdateMode() == ShadowUpdateType::SHADOWUPDATE_NONE,
        "a one-shot shadow is consumed once it renders");

    // The second visible spot leaves the view: one light left, a 1x1 split, a new slot.
    entities[1]->setLocalPosition(1.5f, 2.0f, kBehind);
    renderFrame();
    check(!lights[1]->visibleThisFrame() && !lights[1]->atlasViewportAllocated(),
        "a light that leaves the view gives up its slot the same frame");
    check(slotIs(lights[0], 1.0f), "the one light left takes the whole atlas");
    check(lights[0]->atlasSlotUpdated(), "its slot changed this frame");
    check(stats.drawCalls().shadow > 0 &&
          lights[0]->shadowUpdateMode() == ShadowUpdateType::SHADOWUPDATE_NONE,
        "and its one-shot shadow was re-armed and rendered into it (" +
        std::to_string(stats.drawCalls().shadow) + " shadow draws)");

    // A frame later nothing changed: the slot is kept, and nothing re-renders.
    renderFrame();
    check(slotIs(lights[0], 1.0f) && !lights[0]->atlasSlotUpdated(), "an unchanged visible set keeps the slot");
    check(stats.drawCalls().shadow == 0, "and its one-shot shadow is not rendered again");

    // Back into view: two lights, 2x2 again.
    entities[1]->setLocalPosition(1.5f, 2.0f, kInFront);
    renderFrame();
    check(slotIs(lights[0], 0.5f) && slotIs(lights[1], 0.5f), "a light coming back into view re-splits the atlas");

    return finish("clustered atlas visibility");
}
