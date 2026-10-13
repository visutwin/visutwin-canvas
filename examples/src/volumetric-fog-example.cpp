// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream camera-frame/volumetric-fog.
//
// The low-poly terrain (scaled 30x) under the helipad sky (mip 3, turned -70 degrees), its clouds
// circling over the valley, and an orange pillar casting a long shaft, in an amber height fog lit
// by a low sun: a directional light with PCSS soft shadows, so the terrain, the trees and the
// clouds cast long shafts through the fog. The camera frame renders ACES, sharpening 0.5, a
// subtle bloom (0.01) and the volumetric fog.
//
// Keys stand in for upstream's controls panel, over the same parameters:
//   F fog on / off     T TAA     S soft (PCSS) / hard (PCF3) shadows
//   [ / ]  density -/+ 0.002     - / =  intensity -/+ 0.1
//   arrows turn the light (pitch, yaw) by 2 degrees
//
// DEVIATIONS:
// - The cloud order is shuffled with a fixed seed rather than Math.random(), so every run has
//   the same layout.
// - Orbit camera: upstream's orbit-camera script focuses the tree "Arbol 2.002" and on the first
//   frame sets distance 420, yaw 304, pitch -5 (inertia 0.2, distanceMax 600). This port starts
//   CameraControls at that pose and clamps zoom to 600.
//
// @credit Terrain Low Poly by Sketchfab, CC BY 4.0
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kCloudSpeed = 0.2f;
}

class VolumetricFogExample final : public ExampleApp
{
public:
    VolumetricFogExample() : ExampleApp({.title = "Volumetric Fog"}) {}

protected:
    bool create() override
    {
        // The skydome
        scene()->setSkyboxMip(3);
        scene()->setSkyboxRotation(Quaternion::fromEulerAngles(0.0f, -70.0f, 0.0f));
        _envAtlas = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (Texture* envAtlas = _envAtlas->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        }

        // The terrain
        _terrainAsset = std::make_unique<Asset>("terrain", AssetType::CONTAINER, assetPath("models/terrain.glb"));
        auto* terrainContainer = _terrainAsset->resourceAs<ContainerResource>();
        if (!terrainContainer) {
            spdlog::error("volumetric-fog needs models/terrain.glb");
            return false;
        }
        auto* terrain = terrainContainer->instantiateRenderEntity();
        terrain->setEngine(engine());
        terrain->setLocalScale(30.0f, 30.0f, 30.0f);
        root()->addChild(terrain);

        // The clouds, which receive no shadow, each cloned three more times
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

        // A large orange pillar, casting a long shaft through the fog
        _pillarMaterial = std::make_shared<StandardMaterial>();
        _pillarMaterial->setDiffuse(Color(1.0f, 0.5f, 0.0f, 1.0f));
        auto* pillar = createPrimitive("box", _pillarMaterial.get(), Vector3(180.0f, 50.0f, 110.0f),
            Vector3(10.0f, 130.0f, 10.0f));
        pillar->setName("pillar");

        // The camera, orbiting a tree in the middle at the first-frame pose
        Vector3 focusPoint(0.0f, 0.0f, 0.0f);
        if (auto* tree = dynamic_cast<Entity*>(terrain->findByName("Arbol 2.002"))) {
            focusPoint = entityBounds(tree).center();
        }
        const Vector3 forward = Quaternion::fromEulerAngles(-5.0f, 304.0f, 0.0f) * Vector3(0.0f, 0.0f, -1.0f);
        auto* camera = createCamera(focusPoint - forward * 420.0f);
        _cameraComponent = camera->findComponent<CameraComponent>();
        if (!_cameraComponent || !_cameraComponent->camera()) {
            return false;
        }
        _cameraComponent->camera()->setClearColor(Color(0.9f, 0.9f, 0.9f, 1.0f));
        _cameraComponent->camera()->setFarClip(1000.0f);
        if (auto* controls = addOrbitControls(camera, focusPoint)) {
            controls->setZoomRange(Vector2(0.0f, 600.0f));
            controls->storeResetState();
        }

        // A directional light casting soft shadows, low above the horizon so the terrain, the
        // trees and the clouds cast long shafts through the fog
        _light = createDirectionalLight(Vector3(_pitch, _yaw, 0.0f), Color(1.0f, 0.95f, 0.85f, 1.0f), 1.5f, true);
        _light->setName("MainLight");
        if (auto* light = _light->findComponent<LightComponent>()) {
            light->setShadowBias(0.3f);
            light->setShadowNormalBias(0.2f);
            light->setShadowResolution(2048);
            light->setShadowDistance(1000.0f);
            light->setPenumbraSize(0.03f);
            light->setPenumbraFalloff(4.0f);
            light->setShadowSamples(16);
            light->setShadowBlockerSamples(16);
            _lightComponent = light;
        }

        // The camera frame: ACES, sharpening, a subtle bloom and the volumetric fog
        _cameraComponent->setToneMapping(TONEMAP_ACES);
        auto rendering = _cameraComponent->rendering();
        rendering.sharpness = 0.5f;
        rendering.bloomIntensity = 0.01f;
        _cameraComponent->setRendering(rendering);
        applySettings();

        spdlog::info("Keys: F fog, T TAA, S soft shadows, [ ] density, - = intensity, arrows light");
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Move the clouds around
        const auto count = static_cast<float>(_clouds.size());
        for (size_t index = 0; index < _clouds.size(); ++index) {
            const float radialOffset = (static_cast<float>(index) / count) * (6.24f / kCloudSpeed);
            const float radius = 9.0f + 4.0f * std::sin(radialOffset);
            const float cloudTime = _time + radialOffset;
            _clouds[index]->setLocalPosition(2.0f + radius * std::sin(cloudTime * kCloudSpeed), 4.0f,
                -5.0f + radius * std::cos(cloudTime * kCloudSpeed));
        }
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_F: _fogEnabled = !_fogEnabled; break;
        case SDLK_T: _taa = !_taa; break;
        case SDLK_S: _softShadows = !_softShadows; break;
        case SDLK_LEFTBRACKET: _density = std::max(0.0f, _density - 0.002f); break;
        case SDLK_RIGHTBRACKET: _density = std::min(0.02f, _density + 0.002f); break;
        case SDLK_MINUS: _intensity = std::max(0.0f, _intensity - 0.1f); break;
        case SDLK_EQUALS: _intensity = std::min(5.0f, _intensity + 0.1f); break;
        case SDLK_UP: _pitch += 2.0f; break;
        case SDLK_DOWN: _pitch -= 2.0f; break;
        case SDLK_LEFT: _yaw -= 2.0f; break;
        case SDLK_RIGHT: _yaw += 2.0f; break;
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
        if (_light) {
            _light->setLocalEulerAngles(_pitch, _yaw, 0.0f);
        }
        _cameraComponent->setTaaEnabled(_taa);
        auto& fog = _cameraComponent->volumetricFog();
        fog.enabled = _fogEnabled;
        fog.tint[0] = 0.96f;
        fog.tint[1] = 0.66f;
        fog.tint[2] = 0.18f;
        fog.density = _density;
        fog.heightBase = -74.0f;
        fog.heightFalloff = 0.01f;
        fog.anisotropy = 0.62f;
        fog.intensity = _intensity;
        fog.ambientColor[0] = 0.94f;
        fog.ambientColor[1] = 0.32f;
        fog.ambientColor[2] = 0.14f;
        fog.ambientIntensity = 0.1f;
        fog.maxDistance = 568.0f;
        fog.steps = 24;
        fog.scale = 0.5f;
        if (_lightComponent) {
            _lightComponent->setShadowType(_softShadows ? SHADOW_PCSS_32F : SHADOW_PCF3_32F);
        }
        spdlog::info("fog {} density {:.3f} intensity {:.2f} TAA {} soft shadows {} light ({:.0f}, {:.0f})",
            _fogEnabled, _density, _intensity, _taa, _softShadows, _pitch, _yaw);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _terrainAsset;
    std::shared_ptr<StandardMaterial> _pillarMaterial;
    std::vector<Entity*> _clouds;
    Entity* _light = nullptr;
    LightComponent* _lightComponent = nullptr;
    CameraComponent* _cameraComponent = nullptr;

    bool _fogEnabled = true;
    bool _taa = false;
    bool _softShadows = true;
    float _density = 0.01f;
    float _intensity = 0.74f;
    float _pitch = 78.0f;
    float _yaw = 120.0f;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(VolumetricFogExample)
