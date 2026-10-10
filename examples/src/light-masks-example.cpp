// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream test/light-masks (a hidden test example).
//
// Which lights reach lightmapped geometry, which reach dynamic geometry, and which only
// reach the lightmap. A light carries three independent mask bits:
//   MASK_BAKE                the light is baked into lightmaps and does nothing at runtime;
//   MASK_AFFECT_LIGHTMAPPED  it lights lightmapped mesh instances at runtime;
//   MASK_AFFECT_DYNAMIC      it lights every other mesh instance at runtime.
// A baked mesh instance wears MASK_AFFECT_LIGHTMAPPED, everything else keeps
// MASK_AFFECT_DYNAMIC, and a light reaches a mesh only where the two masks share a bit.
//
// The scene: a dark 13 x 13 lightmapped ground, a back row of five lightmapped primitives
// and a front row of the same five, dynamic, bobbing and turning so it is plain which row
// is lit in real time. One material for all of them.
//
// The lights, IN THIS ORDER, which is what the test is about:
//   1. blue directional, dynamic only        (mask 1)
//   2. red directional, lightmapped only     (mask 2; a runtime light, not baked)
//   3. white directional, both, PCF3 shadow  (mask 3)
//   4. green omni, bake only, shadowed       (mask 4)
// The lights that survive a draw's mask are packed into the shader's light slots from 0,
// so one slot holds a different light depending on the mask being drawn: a lightmapped
// draw sees light 2 in slot 0 and light 3 in slot 1, a dynamic draw light 1 in slot 0 and
// light 3 in slot 1. Neither mask sees its lights as a contiguous run of the creation
// order, the awkward case for anything that indexes per-light data (light 3's shadow
// included). Reordering the lights stops the example testing that.
//
// Keys stand in for upstream's control panel, each a toggle, all on at start:
//   1  dynamic only    2  lightmapped only    3  affect all
//   4  bake only (re-bakes: a baked contribution changes only with the lightmap)
//
// DEVIATIONS:
// - The bake is the GpuLightmapper, which renders over a frame or more; upstream's bakes
//   synchronously. Its options carry upstream's scene.lightmap* values.
// - There is no `lightmapped` or `castShadowsLightmap` render component setting: the
//   lightmapped instances get MASK_AFFECT_LIGHTMAPPED here and are handed to the bake, and
//   the ground, which casts into the lightmap but not at runtime, casts shadows only while
//   a bake runs.
// - The omni's shadow bias is 0.05 rather than upstream's 0.2, as in the other lightmap
//   examples (0.2 pushes small casters out of their baked shadows); light 3 keeps 0.2.
// - Upstream's orbit-camera script becomes CameraControls in orbit mode, zoom capped at
//   40. The script frames its focus entity (the ground) on start: it orbits the ground's
//   centre at 6.5 x 1.5 / sin(22.5 degrees) = 25.5 units, looking along the authored
//   (0, 5, 12) direction, which is where this camera starts (the pivot upstream passes is
//   not one of the script's attributes, so it has no effect there either).
//
#include <cmath>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/lightmapper/gpuLightmapper.h"
#include "platform/input/inputConstants.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr const char* kShapes[] = {"box", "sphere", "capsule", "cone", "cylinder"};
    constexpr float kRowSpacing = 2.4f;
}

class LightMasksExample final : public ExampleApp
{
public:
    LightMasksExample()
        : ExampleApp({.title = "Light Masks"}) {}

protected:
    bool create() override
    {
        // Dark, so each light's contribution is unambiguous.
        scene()->setAmbientLight(0.05f, 0.05f, 0.06f);

        _material = std::make_shared<StandardMaterial>();
        _material->setGloss(0.5f);
        _material->setMetalness(0.2f);
        _material->setUseMetalness(true);

        // The ground is lightmapped, so it shows the bake as well as the runtime lights
        // that reach lightmapped geometry.
        auto* ground = createPrimitive("plane", _material.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(13.0f, 1.0f, 13.0f));
        ground->setName("ground");
        _groundRender = ground->findComponent<RenderComponent>();
        _groundRender->setCastShadows(false);
        addLightmapped(_groundRender);

        // Two rows of the same five shapes, differing only in being lightmapped: the back
        // row is baked, the front row stays dynamic.
        constexpr int shapeCount = static_cast<int>(std::size(kShapes));
        for (int i = 0; i < shapeCount; ++i) {
            const float x = (static_cast<float>(i) - static_cast<float>(shapeCount - 1) * 0.5f) * kRowSpacing;

            auto* baked = createPrimitive(kShapes[i], _material.get(), Vector3(x, 0.7f, -2.8f));
            baked->setName(std::string("baked-") + kShapes[i]);
            addLightmapped(baked->findComponent<RenderComponent>());

            auto* dynamic = createPrimitive(kShapes[i], _material.get(), Vector3(x, 0.7f, 2.8f));
            dynamic->setName(std::string("dynamic-") + kShapes[i]);
            _dynamicEntities.push_back(dynamic);
        }

        // The four lights, in the order the header explains.

        // 1. Dynamic only: the front row, never the lightmapped geometry.
        _dynamicOnlyLight = createDirectionalLight(Vector3(35.0f, 30.0f, 0.0f),
            Color(0.2f, 0.4f, 1.0f), 1.3f, false);
        _dynamicOnlyLight->setName("light-dynamic-only");
        _dynamicOnlyLight->findComponent<LightComponent>()->setMask(MASK_AFFECT_DYNAMIC);

        // 2. Lightmapped only: a runtime light on the back row and the ground, evaluated
        // every frame, not baked.
        _lightmappedOnlyLight = createDirectionalLight(Vector3(35.0f, -60.0f, 0.0f),
            Color(1.0f, 0.25f, 0.2f), 1.0f, false);
        _lightmappedOnlyLight->setName("light-lightmapped-only");
        _lightmappedOnlyLight->findComponent<LightComponent>()->setMask(MASK_AFFECT_LIGHTMAPPED);

        // 3. Affect all: both rows, with a shadow, so the per-light shadow data is read
        // from a different slot by each mask.
        _affectAllLight = createDirectionalLight(Vector3(42.0f, 22.0f, 0.0f),
            Color(1.0f, 0.95f, 0.85f), 0.9f, true);
        _affectAllLight->setName("light-all");
        if (auto* light = _affectAllLight->findComponent<LightComponent>()) {
            light->setMask(MASK_AFFECT_DYNAMIC | MASK_AFFECT_LIGHTMAPPED);
            light->setShadowType(SHADOW_PCF3_32F);
            light->setShadowResolution(2048);
            light->setShadowDistance(40.0f);
            light->setShadowNormalBias(0.05f);
            light->setShadowBias(0.2f);
        }

        // 4. Bake only: in the lightmap and nowhere else. It takes no runtime light slot,
        // so toggling it changes nothing until the re-bake.
        _bakeOnlyLight = new Entity();
        _bakeOnlyLight->setName("light-bake-only");
        _bakeOnlyLight->setEngine(engine());
        if (auto* light = static_cast<LightComponent*>(_bakeOnlyLight->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_OMNI);
            light->setColor(Color(0.3f, 1.0f, 0.4f));
            light->setIntensity(4.0f);
            light->setRange(11.0f);
            light->setMask(MASK_BAKE);
            light->setCastShadows(true);
            light->setShadowResolution(512);
            light->setShadowType(SHADOW_PCF3_32F);
            light->setShadowNormalBias(0.05f);
            light->setShadowBias(0.05f);   // see the DEVIATION in the header
        }
        _bakeOnlyLight->setLocalPosition(-3.5f, 2.0f, -2.8f);
        root()->addChild(_bakeOnlyLight);

        // Where upstream's orbit script puts the camera on start (see the header).
        const Vector3 pivot(0.0f, 0.0f, 0.0f);
        const float sinHalfFov = std::sin(22.5f * 3.14159265358979f / 180.0f);
        auto* camera = createCamera(Vector3(0.0f, 5.0f, 12.0f).normalized() * (6.5f * 1.5f / sinHalfFov));
        camera->setName("camera");
        if (auto* cameraComponent = camera->findComponent<CameraComponent>();
            cameraComponent && cameraComponent->camera()) {
            cameraComponent->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));
            cameraComponent->camera()->setFarClip(100.0f);
            cameraComponent->camera()->setNearClip(0.05f);
        }
        camera->lookAt(pivot);
        if (CameraControls* controls = addOrbitControls(camera, pivot)) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(0.0f, 40.0f));
            controls->storeResetState();
        }

        _baker = std::make_unique<GpuLightmapper>(engine());
        startBake();

        spdlog::info("Keys: 1 dynamic only, 2 lightmapped only, 3 affect all, "
                     "4 bake only (re-bakes), Esc quits");
        return true;
    }

    void update(const float dt) override
    {
        handleKeys();

        // Bob and turn the dynamic row, to make it obvious which row is lit in real time.
        _time += dt;
        for (size_t i = 0; i < _dynamicEntities.size(); ++i) {
            Entity* entity = _dynamicEntities[i];
            const Vector3 position = entity->localPosition();
            entity->setLocalPosition(position.getX(),
                0.7f + std::sin(_time * 1.5f + static_cast<float>(i) * 0.7f) * 0.3f, position.getZ());
            entity->rotate(0.0f, dt * 30.0f, 0.0f);
        }
    }

    void postRender() override
    {
        if (_baker && _baker->baking() && _baker->update()) {
            // The ground casts into the lightmap only.
            _groundRender->setCastShadows(false);
            applyRuntimeLights();
            spdlog::info("Light masks: baked {} mesh instance(s)", _bakeTargets.size());
        }
    }

    void destroy() override
    {
        _baker.reset();
    }

private:
    // Marks a render component's mesh instances lightmapped: they take the lightmapped
    // mask now, as they will after the bake, and are handed to the bake.
    void addLightmapped(RenderComponent* render)
    {
        if (!render) {
            return;
        }
        for (auto* meshInstance : render->meshInstances()) {
            meshInstance->setMask(MASK_AFFECT_LIGHTMAPPED);
            _bakeTargets.push_back(meshInstance);
        }
    }

    void startBake()
    {
        // The bake takes only the light with MASK_BAKE (light 4); it switches the others
        // off for its frames and back on when it is done.
        applyRuntimeLights();
        _groundRender->setCastShadows(true);

        GpuLightmapper::Options options;
        options.lightmapSizeMultiplier = 32.0f;     // scene.lightmapSizeMultiplier
        options.lightmapMaxResolution = 2048;       // scene.lightmapMaxResolution
        options.lightmapFilterEnabled = true;       // scene.lightmapFilterEnabled
        options.lightmapFilterRange = 5.0f;         // scene.lightmapFilterRange
        options.lightmapFilterSmoothness = 0.1f;    // scene.lightmapFilterSmoothness
        options.bakeCameraTarget = Vector3(0.0f, 0.0f, 0.0f);
        options.bakeCameraDistance = 20.0f;
        _baker->bake(_bakeTargets, options);
    }

    // Each light to its toggle. A toggle during a bake is applied again when it finishes,
    // since the bake restores the states it started from.
    void applyRuntimeLights()
    {
        _dynamicOnlyLight->setEnabled(_dynamicOnly);
        _lightmappedOnlyLight->setEnabled(_lightmappedOnly);
        _affectAllLight->setEnabled(_affectAll);
        _bakeOnlyLight->setEnabled(_bakeOnly);
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        if (keyboard->wasPressed(Key::Digit1)) {
            _dynamicOnly = !_dynamicOnly;
            spdlog::info("Dynamic only: {}", _dynamicOnly ? "on" : "off");
            applyRuntimeLights();
        }
        if (keyboard->wasPressed(Key::Digit2)) {
            _lightmappedOnly = !_lightmappedOnly;
            spdlog::info("Lightmapped only: {}", _lightmappedOnly ? "on" : "off");
            applyRuntimeLights();
        }
        if (keyboard->wasPressed(Key::Digit3)) {
            _affectAll = !_affectAll;
            spdlog::info("Affect all: {}", _affectAll ? "on" : "off");
            applyRuntimeLights();
        }
        if (keyboard->wasPressed(Key::Digit4)) {
            _bakeOnly = !_bakeOnly;
            spdlog::info("Bake only: {} (re-baking)", _bakeOnly ? "on" : "off");
            // A baked contribution changes only when the lightmap is regenerated.
            startBake();
        }
    }

    std::shared_ptr<StandardMaterial> _material;
    RenderComponent* _groundRender = nullptr;
    std::vector<MeshInstance*> _bakeTargets;
    std::vector<Entity*> _dynamicEntities;
    std::unique_ptr<GpuLightmapper> _baker;

    Entity* _dynamicOnlyLight = nullptr;
    Entity* _lightmappedOnlyLight = nullptr;
    Entity* _affectAllLight = nullptr;
    Entity* _bakeOnlyLight = nullptr;

    float _time = 0.0f;

    // The control panel's toggles, all on at start.
    bool _dynamicOnly = true;
    bool _lightmappedOnly = true;
    bool _affectAll = true;
    bool _bakeOnly = true;
};

VISUTWIN_EXAMPLE_MAIN(LightMasksExample)
