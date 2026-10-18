// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.10.2026
//
// LensFlareEffect through the real engine on a stub device: what it resolves for each flare
// every frame (its source in normalized device coordinates, the fade from the screen edge, the
// distance and a spot light's cone, the depth the occlusion tests at, the light's colour
// decoded), the elements built from the looks (copies spread along the axis, an unknown shape
// skipped), the ones drawn, when it takes part, and its passes at the postResolve stage (a shape
// copy per layer, the occlusion, the sprites), which a library of another size rebuilds.
//
// A flare in the wrong place or faded wrongly still looks like a lens flare; nothing in a
// frame compared with nothing says it is wrong.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/graphics/lensFlareEffect.h"
#include "scene/graphics/renderPassCameraFrame.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    Entity* addEntity(Engine& engine)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        engine.root()->addChild(std::move(owned));
        return entity;
    }

    float smoothstep(const float a, const float b, const float x)
    {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
        .size = {64, 48}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<CameraComponentSystem, LightComponentSystem>(device);

    // The camera at the origin, looking down -Z.
    Entity* cameraEntity = addEntity(*engine);
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    camera->camera()->setNearClip(0.1f);
    camera->camera()->setFarClip(100.0f);

    auto lensFlare = std::make_shared<LensFlareEffect>();
    check(!lensFlare->active(), "with no flare the effect takes no part");
    lensFlare->setShapes({64, true, {{"glow", nullptr}, {"disc", nullptr}}});

    auto look = std::make_shared<LensFlareLook>();
    look->elements.push_back({.shape = "glow", .size = 0.2f});
    look->elements.push_back({.shape = "disc", .offset = 0.5f, .size = 0.05f, .count = 3, .spread = 0.4f});
    look->elements.push_back({.shape = "missing"});

    // 0: straight ahead; 1: behind; 2: near the right edge; 3: a direction ahead; 4: a distance
    // fade halfway; 5: a spot light facing the camera.
    auto ahead = std::make_shared<LensFlare>(look, LensFlareSource{.position = Vector3(0.0f, 0.0f, -10.0f)});
    ahead->occlusionOffset = 0.5f;
    auto behind = std::make_shared<LensFlare>(look, LensFlareSource{.position = Vector3(0.0f, 0.0f, 10.0f)});
    auto edge = std::make_shared<LensFlare>(look, LensFlareSource{.position = Vector3(5.5f, 0.0f, -10.0f)});
    auto sun = std::make_shared<LensFlare>(look, LensFlareSource{.direction = Vector3(0.0f, 0.0f, -2.0f)});
    auto distant = std::make_shared<LensFlare>(look, LensFlareSource{.position = Vector3(0.0f, 0.0f, -10.0f)});
    distant->fadeStart = 5.0f;
    distant->fadeEnd = 15.0f;

    Entity* spotEntity = addEntity(*engine);
    auto* spot = static_cast<LightComponent*>(spotEntity->addComponent<LightComponent>());
    spot->setType(LightType::LIGHTTYPE_SPOT);
    spot->setColor(Color(1.0f, 0.5f, 0.0f, 1.0f));
    spot->setInnerConeAngle(20.0f);
    spot->setOuterConeAngle(30.0f);
    spotEntity->setLocalPosition(0.0f, 0.0f, -10.0f);
    spotEntity->setLocalEulerAngles(-90.0f, 0.0f, 0.0f);   // -Y, its beam, towards the camera
    auto spotFlare = std::make_shared<LensFlare>(look, LensFlareSource{.light = spot});

    for (const auto& flare : {ahead, behind, edge, sun, distant, spotFlare}) {
        lensFlare->add(flare);
    }
    check(lensFlare->active(), "with a flare it does");
    lensFlare->intensity = 0.0f;
    check(!lensFlare->active(), "not at intensity 0");
    lensFlare->intensity = 1.0f;
    camera->addFrameEffect(lensFlare);

    engine->start();
    const auto frame = [&] {
        engine->update(1.0f / 60.0f);
        engine->render();
    };
    frame();
    frame();

    std::cout << "passes\n";
    {
        const auto& passes = camera->cameraFrame()->effectPasses().postResolve;
        check(passes.size() == 4, "a shape copy per layer, the occlusion and the sprites");
        check(passes.size() == 4 && passes[0]->name() == "RenderPassLensFlare-shape" &&
              passes[2]->name() == "RenderPassLensFlare-occlusion" && passes[3]->name() == "RenderPassLensFlare-sprite",
            "in that order");
        check(passes.size() == 4 && !passes[0]->enabled() && !passes[1]->enabled(),
            "the shape copies run once, after the library was set");
        check(lensFlare->shapeTexture() && lensFlare->shapeTexture()->getArrayLength() == 2 &&
              lensFlare->shapeTexture()->width() == 64, "into a texture array of the library's size");
        check(lensFlare->flareTexture() && lensFlare->flareTexture()->width() == 32 &&
              lensFlare->flareTexture()->height() == 24, "the flares drawn at half the scene's size");
    }

    std::cout << "elements\n";
    {
        check(lensFlare->elementCount() == 6 * 4, "per flare: the glow and three copies of the disc; no unknown shape");
    }

    const auto& states = lensFlare->flareStates();
    check(states.size() == 6, "a state per flare");
    if (states.size() == 6) {
        std::cout << "sources\n";
        check(near(states[0].x, 0.0f, 1e-5f) && near(states[0].y, 0.0f, 1e-5f) && states[0].inFront &&
              states[0].onScreen && near(states[0].fade, 1.0f, 1e-6f), "ahead: at the centre, on screen, unfaded");
        check(near(states[0].sourceDepth, 9.5f, 1e-4f), "tested at its distance less the occlusion offset");
        check(!states[1].inFront && states[1].fade == 0.0f && !states[1].onScreen, "behind: faded out");
        const float m = std::abs(states[2].x);
        check(m > 0.8f && m < 1.2f, "near the edge: inside the fade band");
        check(near(states[2].fade, 1.0f - smoothstep(0.8f, 1.2f, m), 1e-5f), "and faded by its band");
        check(states[3].inFront && near(states[3].x, 0.0f, 1e-5f) && near(states[3].sourceDepth, 98.0f, 1e-3f),
            "a direction: at the centre, tested near the far clip");
        check(near(states[4].fade, 0.5f, 1e-5f), "the distance fade halfway between its two distances");
        check(near(states[5].fade, 1.0f, 1e-5f) && states[5].inFront, "a spot light facing the camera");
        check(near(states[5].color.r, 1.0f, 1e-5f) && near(states[5].color.g, std::pow(0.5f, 2.2f), 1e-4f) &&
              states[5].color.b == 0.0f, "tinted by the light's colour, decoded");

        std::cout << "drawn\n";
        check(lensFlare->drawnElementCount() == 5 * 4, "every flare's elements but the one behind");
    }

    std::cout << "a spot light turned away, and disabled\n";
    {
        spotEntity->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
        frame();
        check(states.size() == 6 && states[5].fade == 0.0f, "outside its cone: faded out");
        spotEntity->setLocalEulerAngles(-90.0f, 0.0f, 0.0f);
        spot->setEnabled(false);
        frame();
        check(states.size() == 6 && states[5].fade == 0.0f, "disabled: faded out");
        spot->setEnabled(true);
    }

    std::cout << "the light's intensity\n";
    {
        spot->setIntensity(2.5f);
        frame();
        check(states.size() == 6 && near(states[5].color.r, 2.5f, 1e-5f), "scales the flare");
        spot->setIntensity(0.0f);
        frame();
        check(states.size() == 6 && states[5].color.r == 0.0f, "and at 0 removes it");
        spotFlare->useLightIntensity = false;
        frame();
        check(states.size() == 6 && near(states[5].color.r, 1.0f, 1e-5f), "unless the flare ignores it");
        spotFlare->useLightIntensity = true;
        spot->setIntensity(1.0f);
    }

    std::cout << "changes\n";
    {
        lensFlare->remove(behind);
        frame();
        check(lensFlare->elementCount() == 5 * 4 && lensFlare->flareStates().size() == 5, "a removed flare is gone");
        auto shorter = std::make_shared<LensFlareLook>();
        shorter->elements.push_back({.shape = "glow"});
        ahead->setLook(shorter);
        frame();
        check(lensFlare->elementCount() == 4 * 4 + 1, "a new look rebuilds the elements");

        lensFlare->setShapes({64, true, {{"glow", nullptr}, {"disc", nullptr}, {"ring", nullptr}}});
        frame();
        frame();
        const auto& passes = camera->cameraFrame()->effectPasses().postResolve;
        check(passes.size() == 5 && lensFlare->shapeTexture()->getArrayLength() == 3,
            "a library of another layer count rebuilds the passes and the array");
    }

    return finish("lens flare effect");
}
