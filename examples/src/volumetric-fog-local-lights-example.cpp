// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Port of upstream graphics/volumetric-fog-local-lights.
//
// The low-poly terrain (scaled 30x) at dusk under a dim helipad sky, with its clouds
// circling over the valley, an orange pillar casting a long shaft, and volumetric
// height fog lit by a dim moon (a shadowed directional light), three sweeping
// shadowed spot lights with a linear falloff (volumetric scattering 15) and four omni
// lights drifting above the terrain, each marked by an emissive sphere. The camera
// frame renders with TAA, ACES, sharpening and a subtle bloom; the fog's local lights
// sample the clustered shadow atlas (2048).
//
// Keys stand in for upstream's control panel:
//   F fog on/off   O omni lights   S spot lights   H spot shadows   A animate lights
//   T TAA          [ / ] fog density -/+           ; / ' local intensity -/+
//
// DEVIATIONS:
// - The cloud order is shuffled with a fixed seed rather than Math.random(), so every run
//   has the same layout (as shadow-cascades does).
// - Orbit camera: upstream's orbit-camera script focuses the tree "Arbol 2.002" and on the
//   first frame sets distance 470, yaw 304, pitch -6 (inertia 0.2, distanceMax 600). This
//   port starts CameraControls at that pose and clamps zoom to 600.
//
// @credit Terrain Low Poly by Sketchfab, CC BY 4.0
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include <framework/assets/asset.h>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kCloudSpeed = 0.2f;
    constexpr float kPi = std::numbers::pi_v<float>;
}

class VolumetricFogLocalLightsExample final: public ExampleApp
{
public:
    VolumetricFogLocalLightsExample()
        : ExampleApp({.title = "Volumetric Fog Local Lights", .width = 1100, .height = 750}) {}

protected:
    bool create() override
    {
        // A dim skydome, to give the scene a dusk feel where the local lights dominate.
        scene()->setSkyboxMip(3);
        scene()->setSkyboxIntensity(0.15f);
        scene()->setSkyboxRotation(Quaternion::fromEulerAngles(0.0f, -70.0f, 0.0f));
        _envAtlas = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        const auto envAtlasResource = _envAtlas->resource();
        if (!envAtlasResource) {
            spdlog::error("Failed to load environment atlas texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));

        // The fog's local lights sample the clustered lighting's shadow atlas; a larger one
        // gives the shadows of the beams more detail.
        scene()->lighting().shadowAtlasResolution = 2048;

        // The terrain
        _terrainAsset = std::make_unique<Asset>("terrain", AssetType::CONTAINER, assetPath("models/terrain.glb"));
        const auto terrainResource = _terrainAsset->resource();
        if (!terrainResource || !std::holds_alternative<ContainerResource*>(*terrainResource)) {
            spdlog::error("Failed to load terrain.glb");
            return false;
        }
        auto* terrain = std::get<ContainerResource*>(*terrainResource)->instantiateRenderEntity();
        if (!terrain) {
            spdlog::error("Failed to instantiate terrain.glb");
            return false;
        }
        terrain->setEngine(engine());
        terrain->setLocalScale(30.0f, 30.0f, 30.0f);
        root()->addChild(terrain);

        // The clouds, which receive no shadow, each cloned three more times.
        const auto srcClouds = terrain->find([](GraphNode* node) {
            const bool isCloud = node->name().find("Icosphere") != std::string::npos;
            if (isCloud) {
                if (auto* entity = dynamic_cast<Entity*>(node)) {
                    if (auto* render = entity->findComponent<RenderComponent>()) {
                        render->setReceiveShadows(false);
                    }
                }
            }
            return isCloud;
        });
        for (auto* node : srcClouds) {
            auto* cloud = dynamic_cast<Entity*>(node);
            if (!cloud || !cloud->parent()) {
                continue;
            }
            _clouds.push_back(cloud);
            for (int i = 0; i < 3; i++) {
                Entity* clone = cloud->clone();
                cloud->parent()->addChild(clone);
                _clouds.push_back(clone);
            }
        }
        std::shuffle(_clouds.begin(), _clouds.end(), std::mt19937(12345u));

        // A large orange pillar, casting a long shaft through the fog.
        _pillarMaterial = std::make_shared<StandardMaterial>();
        _pillarMaterial->setDiffuse(Color(1.0f, 0.5f, 0.0f, 1.0f));
        auto* pillar = new Entity();
        pillar->setName("pillar");
        pillar->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(pillar->addComponent<RenderComponent>())) {
            render->setMaterial(_pillarMaterial.get());
            render->setType("box");
        }
        pillar->setLocalScale(10.0f, 130.0f, 10.0f);
        pillar->setLocalPosition(180.0f, 50.0f, 110.0f);
        root()->addChild(pillar);

        // The camera, orbiting a tree in the middle of the valley at the first-frame
        // pose: distance 470, yaw 304, pitch -6.
        Vector3 focusPoint(0.0f, 0.0f, 0.0f);
        if (auto* tree = dynamic_cast<Entity*>(terrain->findByName("Arbol 2.002"))) {
            focusPoint = entityBounds(tree).center();
        }
        const Vector3 forward = Quaternion::fromEulerAngles(-6.0f, 304.0f, 0.0f) * Vector3(0.0f, 0.0f, -1.0f);
        auto* camera = createCamera(focusPoint - forward * 470.0f);
        _cameraComponent = camera->findComponent<CameraComponent>();
        if (_cameraComponent && _cameraComponent->camera()) {
            _cameraComponent->camera()->setClearColor(Color(0.02f, 0.03f, 0.06f, 1.0f));
            _cameraComponent->camera()->setFarClip(1000.0f);
        }
        if (auto* controls = addOrbitControls(camera, focusPoint)) {
            controls->setZoomRange(Vector2(0.0f, 600.0f));
            controls->storeResetState();
        }

        // A dim directional light acting as the moon, still casting long shafts through the fog.
        auto* moon = createDirectionalLight(Vector3(65.0f, 20.0f, 0.0f), Color(0.6f, 0.75f, 1.0f, 1.0f), 0.7f, true);
        moon->setName("MainLight");
        if (auto* light = moon->findComponent<LightComponent>()) {
            light->setShadowBias(0.3f);
            light->setShadowNormalBias(0.2f);
            light->setShadowType(SHADOW_PCF3_32F);
            light->setShadowResolution(2048);
            light->setShadowDistance(1000.0f);
        }

        // Sweeping spot lights with a linear falloff, which keeps their beams bright over the
        // whole range of the light. Their narrow beams cross only a short part of each view
        // ray, so they scatter considerably more than the omni lights' wide volumes.
        const std::array<Color, 3> spotColors = {Color(0.3f, 0.7f, 1.0f, 1.0f),
            Color(1.0f, 0.35f, 0.7f, 1.0f), Color(1.0f, 0.8f, 0.35f, 1.0f)};
        for (size_t index = 0; index < spotColors.size(); ++index) {
            auto* spot = new Entity();
            spot->setName("Spot-" + std::to_string(index));
            spot->setEngine(engine());
            auto* light = static_cast<LightComponent*>(spot->addComponent<LightComponent>());
            light->setType(LightType::LIGHTTYPE_SPOT);
            light->setColor(spotColors[index]);
            light->setIntensity(0.7f);
            light->setRange(400.0f);
            light->setFalloffMode(LightFalloff::LIGHTFALLOFF_LINEAR);
            light->setInnerConeAngle(6.0f);
            light->setOuterConeAngle(18.0f);
            light->setCastShadows(true);
            light->setShadowBias(0.3f);
            light->setShadowNormalBias(0.2f);
            light->setVolumetricScattering(15.0f);
            root()->addChild(spot);
            _spots.push_back(spot);
            _spotLights.push_back(light);
        }

        // Omni lights drifting just above the terrain, each with a small emissive sphere.
        const std::array<Color, 4> omniColors = {Color(1.0f, 0.5f, 0.15f, 1.0f), Color(0.4f, 1.0f, 0.6f, 1.0f),
            Color(0.6f, 0.5f, 1.0f, 1.0f), Color(1.0f, 0.3f, 0.3f, 1.0f)};
        for (size_t index = 0; index < omniColors.size(); ++index) {
            auto* omni = new Entity();
            omni->setName("Omni-" + std::to_string(index));
            omni->setEngine(engine());
            auto* light = static_cast<LightComponent*>(omni->addComponent<LightComponent>());
            light->setType(LightType::LIGHTTYPE_OMNI);
            light->setColor(omniColors[index]);
            light->setIntensity(0.35f);
            light->setRange(150.0f);
            light->setFalloffMode(LightFalloff::LIGHTFALLOFF_LINEAR);
            light->setCastShadows(false);

            auto bulb = std::make_shared<StandardMaterial>();
            bulb->setEmissive(omniColors[index]);
            _bulbMaterials.push_back(bulb);
            if (auto* render = static_cast<RenderComponent*>(omni->addComponent<RenderComponent>())) {
                render->setMaterial(bulb.get());
                render->setType("sphere");
                render->setCastShadows(false);
            }
            omni->setLocalScale(8.0f, 8.0f, 8.0f);
            root()->addChild(omni);
            _omnis.push_back(omni);
        }

        // The camera frame: TAA, a subtle bloom and the volumetric fog.
        if (_cameraComponent) {
            _cameraComponent->setToneMapping(TONEMAP_ACES);
            auto rendering = _cameraComponent->rendering();
            rendering.sharpness = 0.5f;
            rendering.bloomIntensity = 0.015f;
            _cameraComponent->setRendering(rendering);

            auto& fog = _cameraComponent->volumetricFog();
            fog.tint[0] = 0.75f;
            fog.tint[1] = 0.85f;
            fog.tint[2] = 1.0f;
            fog.heightBase = -74.0f;
            fog.heightFalloff = 0.008f;
            fog.ambientColor[0] = 0.2f;
            fog.ambientColor[1] = 0.3f;
            fog.ambientColor[2] = 0.5f;
            fog.ambientIntensity = 0.018f;
            fog.maxDistance = 700.0f;
        }
        applySettings();

        spdlog::info("Keys: F fog, O omni, S spot, H spot shadows, A animate, T TAA, [ ] density, ; ' local intensity");
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        if (_animate) {
            _lightTime += dt;
        }

        // Move the clouds around.
        const auto count = static_cast<float>(_clouds.size());
        for (size_t index = 0; index < _clouds.size(); ++index) {
            const float radialOffset = (static_cast<float>(index) / count) * (6.24f / kCloudSpeed);
            const float radius = 9.0f + 4.0f * std::sin(radialOffset);
            const float cloudTime = _time + radialOffset;
            _clouds[index]->setLocalPosition(2.0f + radius * std::sin(cloudTime * kCloudSpeed), 4.0f,
                -5.0f + radius * std::cos(cloudTime * kCloudSpeed));
        }

        // Sweep the spot lights over the terrain, orbiting high above it and aiming at a target
        // which circles the valley at a different rate. A spot shines down its negative Y axis,
        // so the entity is turned after aiming it.
        for (size_t index = 0; index < _spots.size(); ++index) {
            const float phase = (static_cast<float>(index) / static_cast<float>(_spots.size())) * kPi * 2.0f;
            _spots[index]->setLocalPosition(250.0f * std::sin(phase + _lightTime * 0.15f), 145.0f,
                250.0f * std::cos(phase + _lightTime * 0.15f));
            const Vector3 target(140.0f * std::sin(phase - _lightTime * 0.4f), 5.0f,
                140.0f * std::cos(phase - _lightTime * 0.4f));
            _spots[index]->lookAt(target, Vector3(1.0f, 0.0f, 0.0f));
            _spots[index]->rotateLocal(90.0f, 0.0f, 0.0f);
        }

        // Drift the omni lights above the terrain.
        for (size_t index = 0; index < _omnis.size(); ++index) {
            const float phase = (static_cast<float>(index) / static_cast<float>(_omnis.size())) * kPi * 2.0f;
            _omnis[index]->setLocalPosition(195.0f * std::sin(phase + _lightTime * 0.25f),
                40.0f + 25.0f * std::sin(phase + _lightTime * 0.7f),
                195.0f * std::cos(phase + _lightTime * 0.25f));
        }
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_F: _fogEnabled = !_fogEnabled; break;
        case SDLK_O: _localOmni = !_localOmni; break;
        case SDLK_S: _localSpot = !_localSpot; break;
        case SDLK_H: _spotShadows = !_spotShadows; break;
        case SDLK_A: _animate = !_animate; break;
        case SDLK_T: _taa = !_taa; break;
        case SDLK_LEFTBRACKET: _density = std::max(0.0f, _density - 0.002f); break;
        case SDLK_RIGHTBRACKET: _density = std::min(0.04f, _density + 0.002f); break;
        case SDLK_SEMICOLON: _localIntensity = std::max(0.0f, _localIntensity - 2.5f); break;
        case SDLK_APOSTROPHE: _localIntensity = std::min(50.0f, _localIntensity + 2.5f); break;
        default: return false;
        }
        applySettings();
        return true;
    }

private:
    // The settings, from the control panel's defaults.
    void applySettings()
    {
        if (!_cameraComponent) {
            return;
        }
        _cameraComponent->setTaaEnabled(_taa);
        auto& fog = _cameraComponent->volumetricFog();
        fog.enabled = _fogEnabled;
        fog.density = _density;
        fog.anisotropy = 0.55f;
        fog.intensity = 0.4f;
        fog.steps = 24;
        fog.scale = 0.5f;
        fog.localOmniLights = _localOmni;
        fog.localSpotLights = _localSpot;
        fog.localIntensity = _localIntensity;
        fog.localSteps = 16;
        for (auto* light : _spotLights) {
            light->setCastShadows(_spotShadows);
        }
        spdlog::info("fog {} density {:.3f} omni {} spot {} local intensity {:.1f} shadows {} TAA {}",
            _fogEnabled, _density, _localOmni, _localSpot, _localIntensity, _spotShadows, _taa);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _terrainAsset;
    std::shared_ptr<StandardMaterial> _pillarMaterial;
    std::vector<std::shared_ptr<StandardMaterial>> _bulbMaterials;
    std::vector<Entity*> _clouds;
    std::vector<Entity*> _spots;
    std::vector<LightComponent*> _spotLights;
    std::vector<Entity*> _omnis;
    CameraComponent* _cameraComponent = nullptr;

    bool _fogEnabled = true;
    float _density = 0.011f;
    bool _taa = true;
    bool _localOmni = true;
    bool _localSpot = true;
    float _localIntensity = 15.0f;
    bool _spotShadows = true;
    bool _animate = true;
    float _time = 0.0f;
    float _lightTime = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(VolumetricFogLocalLightsExample)
