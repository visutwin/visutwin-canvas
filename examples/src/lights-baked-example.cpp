// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/lights-baked.
//
// A lightmapped 20 x 20 ground plane and an 8 x 8 grid (spacing 1) of half-scale
// primitives at height 1.5, cycling box, cone, cylinder, sphere, capsule, torus by grid
// index, all sharing one metalness material (gloss 0.6, metalness 0.4). Three lights are
// bake-only (they reach nothing at runtime, only the lightmaps), each casting PCF3
// shadows into the bake (normal offset 0.05, shadow distance 50):
//   - a green omni, range 7, at (-2.5, 4, -2.5), 512 shadow map, shown by an emissive
//     green sphere on the same entity;
//   - a red spot, range 15, cones 30 / 45 degrees, at (2.5, 4, 2.5) pointing straight
//     down, 512 shadow map, shown by an emissive red cone child;
//   - a yellow directional, intensity 0.33, at (0, 4, 0) aimed by (60, -45, 0), 2048
//     shadow map, shown by a slim emissive yellow cylinder child.
// The markers are unlit (emissive intensity 5) and cast no shadows. The scene ambient is
// black and there is no sky, so the lightmaps are everything the grid and the ground show.
// The bake is BAKE_COLOR at lightmap size multiplier 32, max resolution 2048. The camera
// clears to (0.4, 0.45, 0.5), near 0.05, far 100, and orbits the scene from where the orbit
// script's frame-on-start puts it.
//
// Keys stand in for upstream's control panel, each a toggle that re-bakes:
//   1  omni light    2  spot light    3  directional light (all on at start)
//   4  soft directional shadows (off at start): the directional light bakes as 15 copies
//      over a 20 degree area, and the lightmap filter runs (range 5, smoothness 0.1)
//
// DEVIATIONS:
// - The bake is the GpuLightmapper, which renders over several frames; upstream's bakes
//   synchronously. Its options carry upstream's scene.lightmap* values, and the soft
//   toggle's bakeNumSamples / bakeArea go to its directional options, which apply to
//   every directional light (there is one).
// - There is no `lightmapped` or `castShadowsLightmap` render component setting: the
//   lightmapped instances take MASK_AFFECT_LIGHTMAPPED and are handed to the bake, and
//   cast shadows only while a bake runs.
// - The lights' shadow bias is 0.05 rather than upstream's 0.2, as in the other lightmap
//   examples (0.2 pushes small casters out of their baked shadows).
// - Upstream's orbit-camera script (inertia 0.2, distanceMax 15) becomes CameraControls in
//   orbit mode, zoom capped at 15. The script frames the whole scene on start: from the
//   authored position (1, 3, -1) it looks at the scene's bounds centre, then backs off along
//   that line to 1.5 x the largest half extent / sin(22.5 degrees), clamped to 15.
//
#include <algorithm>
#include <array>
#include <cmath>
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
    // All render component primitive shape types.
    constexpr std::array<const char*, 6> kShapes = {"box", "cone", "cylinder", "sphere", "capsule", "torus"};

    constexpr int kGridSize = 8;
    constexpr float kSpacing = 1.0f;

    // Position for lights: halfway between the centre and the corners.
    constexpr float kLightOffset = 2.5f;
    constexpr float kLightHeight = 4.0f;
}

class LightsBakedExample final: public ExampleApp
{
public:
    LightsBakedExample(): ExampleApp({.title = "Lights Baked"}) {}

protected:
    bool create() override
    {
        // The material used on the geometry.
        _material = std::make_shared<StandardMaterial>();
        _material->setGloss(0.6f);
        _material->setMetalness(0.4f);
        _material->setUseMetalness(true);

        // Ground plane.
        auto* ground = createPrimitive("plane", _material.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(20.0f, 20.0f, 20.0f));
        ground->setName("ground");
        addLightmapped(ground);

        // Objects in an 8 x 8 grid.
        constexpr float startOffset = -(static_cast<float>(kGridSize - 1) * kSpacing) / 2.0f;
        for (int x = 0; x < kGridSize; ++x) {
            for (int z = 0; z < kGridSize; ++z) {
                // Deterministic shape based on the grid position.
                const char* shape = kShapes[static_cast<size_t>(x + z * kGridSize) % kShapes.size()];
                const float posX = startOffset + static_cast<float>(x) * kSpacing;
                const float posZ = startOffset + static_cast<float>(z) * kSpacing;
                auto* entity = createPrimitive(shape, _material.get(), Vector3(posX, 1.5f, posZ),
                    Vector3(0.5f, 0.5f, 0.5f));
                addLightmapped(entity);
            }
        }

        // Emissive materials for the light markers.
        _markerGreen = createMarkerMaterial(Color(0.0f, 1.0f, 0.0f, 1.0f));
        _markerRed = createMarkerMaterial(Color(1.0f, 0.0f, 0.0f, 1.0f));
        _markerYellow = createMarkerMaterial(Color(1.0f, 1.0f, 0.0f, 1.0f));

        // An omni light configured as a baked light, with a visible sphere on its entity.
        _lightOmni = createBakeLight("omni", LightType::LIGHTTYPE_OMNI, Color(0.0f, 1.0f, 0.0f, 1.0f),
            1.0f, 512);
        if (auto* light = _lightOmni->findComponent<LightComponent>()) {
            light->setRange(7.0f);
        }
        addMarker(_lightOmni, "sphere", _markerGreen.get());
        _lightOmni->setLocalPosition(-kLightOffset, kLightHeight, -kLightOffset);
        root()->addChild(_lightOmni);

        // A spot light configured as a baked light, pointing straight down (a spot shines
        // along its -Y axis), with a visible cone child.
        _lightSpot = createBakeLight("spot", LightType::LIGHTTYPE_SPOT, Color(1.0f, 0.0f, 0.0f, 1.0f),
            1.0f, 512);
        if (auto* light = _lightSpot->findComponent<LightComponent>()) {
            light->setRange(15.0f);
            light->setInnerConeAngle(30.0f);
            light->setOuterConeAngle(45.0f);
        }
        _lightSpot->setLocalPosition(kLightOffset, kLightHeight, kLightOffset);
        _lightSpot->setLocalEulerAngles(0.0f, 0.0f, 0.0f);
        root()->addChild(_lightSpot);
        addMarkerChild(_lightSpot, "cone", _markerRed.get(), Vector3(1.0f, 1.0f, 1.0f));

        // A directional light configured as a baked light, with a slim visible cylinder child.
        _lightDirectional = createBakeLight("directional", LightType::LIGHTTYPE_DIRECTIONAL,
            Color(1.0f, 1.0f, 0.0f, 1.0f), 0.33f, 2048);
        root()->addChild(_lightDirectional);
        _lightDirectional->setLocalPosition(0.0f, kLightHeight, 0.0f);
        _lightDirectional->setLocalEulerAngles(60.0f, -45.0f, 0.0f);
        addMarkerChild(_lightDirectional, "cylinder", _markerYellow.get(), Vector3(0.2f, 1.0f, 0.2f));

        // The start pose (see the header): framing everything the scene renders, the
        // light markers included.
        BoundingBox sceneBounds = entityBounds(_sceneEntities.front());
        for (Entity* entity : _sceneEntities) {
            sceneBounds.add(entityBounds(entity));
        }
        for (Entity* light : {_lightOmni, _lightSpot, _lightDirectional}) {
            sceneBounds.add(entityBounds(light));
        }
        const Vector3 pivot = sceneBounds.center();
        const Vector3 halfExtents = sceneBounds.halfExtents();
        const float radius = std::max({halfExtents.getX(), halfExtents.getY(), halfExtents.getZ()});
        const float distance = std::min(radius * 1.5f / std::sin(0.5f * 45.0f * DEG_TO_RAD), 15.0f);

        const Vector3 direction = (Vector3(1.0f, 3.0f, -1.0f) - pivot).normalized();
        auto* camera = createCamera(pivot + direction * distance);
        camera->lookAt(pivot);
        if (auto* cameraComponent = camera->findComponent<CameraComponent>();
            cameraComponent && cameraComponent->camera()) {
            cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
            cameraComponent->camera()->setFarClip(100.0f);
            cameraComponent->camera()->setNearClip(0.05f);
        }
        if (CameraControls* controls = addOrbitControls(camera, pivot)) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(0.0f, 15.0f));
            controls->storeResetState();
        }

        // The initial bake.
        _baker = std::make_unique<GpuLightmapper>(engine());
        startBake();

        spdlog::info("Keys (each re-bakes): 1 omni, 2 spot, 3 directional, 4 soft directional shadows");
        return true;
    }

    void update(float) override
    {
        handleKeys();
    }

    void postRender() override
    {
        if (_baker && _baker->baking() && _baker->update()) {
            // The lightmapped geometry casts into the lightmap only.
            setBakeCasters(false);
            spdlog::info("Lights baked: {} mesh instance(s)", _bakeTargets.size());
        }
    }

    void destroy() override
    {
        _baker.reset();
    }

private:
    std::shared_ptr<StandardMaterial> createMarkerMaterial(const Color& emissive) const
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setEmissive(emissive);
        material->setEmissiveIntensity(5.0f);
        material->setUseLighting(false);
        return material;
    }

    // A bake-only light: it reaches neither dynamic nor lightmapped geometry at runtime
    // and is baked into the lightmaps, which is MASK_BAKE alone.
    Entity* createBakeLight(const char* name, const LightType type, const Color& color,
        const float intensity, const int shadowResolution) const
    {
        auto* entity = new Entity();
        entity->setName(name);
        entity->setEngine(engine());
        if (auto* light = static_cast<LightComponent*>(entity->addComponent<LightComponent>())) {
            light->setType(type);
            light->setColor(color);
            light->setIntensity(intensity);
            light->setMask(MASK_BAKE);
            light->setCastShadows(true);
            light->setShadowNormalBias(0.05f);
            light->setShadowBias(0.05f);   // see the DEVIATION in the header
            light->setShadowDistance(50.0f);
            light->setShadowResolution(shadowResolution);
            light->setShadowType(SHADOW_PCF3_32F);
        }
        return entity;
    }

    // An unlit marker render component on the light's own entity.
    static void addMarker(Entity* entity, const char* type, Material* material)
    {
        if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
            render->setMaterial(material);
            render->setType(type);
            render->setCastShadows(false);
        }
    }

    // An unlit marker on a child entity of the light.
    void addMarkerChild(Entity* parent, const char* type, Material* material, const Vector3& scale) const
    {
        auto* child = new Entity();
        child->setEngine(engine());
        addMarker(child, type, material);
        child->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
        parent->addChild(child);
    }

    // Marks an entity's mesh instances lightmapped: they take the lightmapped mask now, as
    // they will after the bake, and are handed to the bake.
    void addLightmapped(Entity* entity)
    {
        _sceneEntities.push_back(entity);
        auto* render = entity->findComponent<RenderComponent>();
        if (!render) {
            return;
        }
        render->setCastShadows(false);
        _bakeCasters.push_back(render);
        for (auto* meshInstance : render->meshInstances()) {
            meshInstance->setMask(MASK_AFFECT_LIGHTMAPPED);
            _bakeTargets.push_back(meshInstance);
        }
    }

    void setBakeCasters(const bool cast) const
    {
        for (auto* render : _bakeCasters) {
            render->setCastShadows(cast);
        }
    }

    void startBake()
    {
        _lightOmni->setEnabled(_omniEnabled);
        _lightSpot->setEnabled(_spotEnabled);
        _lightDirectional->setEnabled(_directionalEnabled);
        setBakeCasters(true);

        GpuLightmapper::Options options;
        options.lightmapSizeMultiplier = 32.0f;     // scene.lightmapSizeMultiplier
        options.lightmapMaxResolution = 2048;       // scene.lightmapMaxResolution
        // The soft toggle: bakeNumSamples 15 over a bakeArea of 20 degrees, and the
        // lightmap filter on.
        options.directionalBakeNumSamples = _soft ? 15 : 1;
        options.directionalBakeArea = _soft ? 20.0f : 0.0f;
        options.lightmapFilterEnabled = _soft;      // scene.lightmapFilterEnabled
        options.lightmapFilterRange = 5.0f;         // scene.lightmapFilterRange
        options.lightmapFilterSmoothness = 0.1f;    // scene.lightmapFilterSmoothness
        options.bakeCameraTarget = Vector3(0.0f, 0.0f, 0.0f);
        options.bakeCameraDistance = 30.0f;
        _baker->bake(_bakeTargets, options);
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        bool bakeSettingChanged = false;
        if (keyboard->wasPressed(Key::Digit1)) {
            _omniEnabled = !_omniEnabled;
            spdlog::info("Omni: {}", _omniEnabled ? "on" : "off");
            bakeSettingChanged = true;
        }
        if (keyboard->wasPressed(Key::Digit2)) {
            _spotEnabled = !_spotEnabled;
            spdlog::info("Spot: {}", _spotEnabled ? "on" : "off");
            bakeSettingChanged = true;
        }
        if (keyboard->wasPressed(Key::Digit3)) {
            _directionalEnabled = !_directionalEnabled;
            spdlog::info("Directional: {}", _directionalEnabled ? "on" : "off");
            bakeSettingChanged = true;
        }
        if (keyboard->wasPressed(Key::Digit4)) {
            _soft = !_soft;
            spdlog::info("Soft directional shadows: {}", _soft ? "on" : "off");
            bakeSettingChanged = true;
        }

        // Re-bake when a relevant setting changed.
        if (bakeSettingChanged) {
            startBake();
        }
    }

    std::shared_ptr<StandardMaterial> _material;
    std::shared_ptr<StandardMaterial> _markerGreen;
    std::shared_ptr<StandardMaterial> _markerRed;
    std::shared_ptr<StandardMaterial> _markerYellow;

    std::unique_ptr<GpuLightmapper> _baker;
    std::vector<MeshInstance*> _bakeTargets;
    std::vector<RenderComponent*> _bakeCasters;
    std::vector<Entity*> _sceneEntities;

    Entity* _lightOmni = nullptr;
    Entity* _lightSpot = nullptr;
    Entity* _lightDirectional = nullptr;

    // The control panel's toggles.
    bool _omniEnabled = true;
    bool _spotEnabled = true;
    bool _directionalEnabled = true;
    bool _soft = false;
};

VISUTWIN_EXAMPLE_MAIN(LightsBakedExample)
