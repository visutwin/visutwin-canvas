// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.10.2026
//
// Port of upstream camera-frame/dynamic-sky.
//
// A dynamic sky over a day and night cycle, rendered by the DynamicSkyEffect of the camera frame:
// an analytic atmosphere, stars, the moon and volumetric clouds drifting with the wind, lighting
// the scene and driving the sun light. Two copies of the tropical island (the second turned 140
// degrees and sunk a little) in the sea of the Water script: a circle of radius 1200 at the
// island's sea level of -0.62 on a layer of its own after the world's transparent sublayer, with
// waves, tropical colours and a depth fade of 3, the island's materials taking its underwater fog
// (EXP 0.068, its colour following the daylight) and caustics; the sun a directional light with four cascades (bias 0.2, normal
// offset 0.05, distance 120, 2048); the camera (fov 60, far 3000) orbiting a point on the island;
// neutral tone mapping, bloom 0.02 over 8 levels above a threshold of 5, and an auto exposure
// (compensation 0.5) adapting from the day to the night. The sky's moon is dimmed to 0.4, its
// clouds 9 km across at 1.8 km, covering 0.42 at quality 0.5 and full resolution, in a wind of 30
// m/s from 45 degrees, the lighting interpolated between its updates and regenerated as the
// clouds drift by 0.005 of their pattern. The hour starts at 5.5 and runs at 0.25 hours a second
// from 05:00 to 20:00, the sun's elevation following a smoothstep curve over the day and its
// azimuth the hour.
//
// Keys stand in for upstream's controls panel:
//   Space  the time running on / off
//   [ / ]  the hour -/+ 0.5
//   C      the cloud type, cycled none / flat / volumetric
//   - / =  the cloud coverage -/+ 0.05
//   Q      the cloud quality, cycled 0 / 0.5 / 1
//   R      the cloud resolution, cycled 0.25 / 0.5 / 1
//   B      bloom on / off
//   E      auto exposure on / off
//   T      TAA on / off
// VISUTWIN_SKY_HOUR=h starts at that hour with the time stopped, and VISUTWIN_SKY_AUTO_EXPOSURE=0
// without the auto exposure (VISUTWIN_SKY_BLOOM=0 without the bloom), for a capture (its histogram arrives a frame or two late, on a frame
// that differs between backends).
//
// DEVIATIONS:
// - the island's textures are the same pixels re-encoded from WebP to PNG
//   (assets/models/tropical-island.credits.txt), which the image decoder here reads.
// - upstream's CameraControls focus point and ranges are kept, through the examples' orbit
//   controls.
//
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#include "core/math/curve.h"
#include "extras/script/cameraControls.h"
#include "extras/script/water.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/render/primitiveGeometry.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/graphics/autoExposureEffect.h"
#include "scene/graphics/dynamicSkyEffect.h"

using namespace visutwin::canvas;

namespace
{
    // The layer of the water, which its internal cameras do not render.
    constexpr int LAYERID_WATER = 100;
}

class DynamicSkyExample final: public ExampleApp
{
public:
    DynamicSkyExample(): ExampleApp({.title = "Dynamic Sky Example"}) {}

protected:
    bool create() override
    {
        _island = std::make_unique<Asset>("island", AssetType::CONTAINER, assetPath("models/tropical-island.glb"));
        _normal = std::make_unique<Asset>("normal", AssetType::TEXTURE, assetPath("textures/normal-map.png"));
        _caustics = std::make_unique<Asset>("caustics", AssetType::TEXTURE, assetPath("textures/caustics.jpg"));
        ContainerResource* container = _island->resourceAs<ContainerResource>();
        if (!container) {
            spdlog::error("Failed to load the island");
            return false;
        }

        // A layer for the water plane, so it can be left out of its reflection and refraction.
        const auto layers = scene()->layers();
        auto waterLayer = std::make_shared<Layer>("Water", LAYERID_WATER);
        layers->insert(waterLayer, layers->getTransparentIndex(layers->getLayerById(LAYERID_WORLD)) + 1);

        // The tropical island, and a second copy close by, turned and sunk a bit deeper.
        Entity* island = container->instantiateRenderEntity();
        root()->addChild(island);
        auto* island2 = new Entity();
        island2->setName("Island2");
        island2->setEngine(engine());
        island2->addChild(container->instantiateRenderEntity());
        island2->setLocalPosition(15.5f, -0.59f, -10.9f);
        island2->setLocalEulerAngles(0.0f, 140.0f, 0.0f);
        root()->addChild(island2);
        for (Entity* entity : {island, island2}) {
            for (auto* render : entity->findComponents<RenderComponent>()) {
                render->setCastShadows(true);
                render->setReceiveShadows(true);
            }
        }

        // The water's underwater fog and caustics on the island (both copies share the materials).
        for (auto* render : island->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                if (Material* material = meshInstance->material()) {
                    Water::applyUnderwaterEffects(*material, true, true);
                }
            }
        }

        // The underwater fog, which the water's fog limits to the fragments below its level.
        scene()->setFogEnabled(true);
        scene()->setFogType(FogType::FOG_EXP);
        scene()->setFogDensity(0.068f);

        // The sun: the dynamic sky sets its direction, colour and intensity by the time of day.
        // Four cascades keep the shadows sharp from close to the camera to the distance.
        _sun = createDirectionalLight(Vector3(0.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        _sun->setName("Sun");
        auto* sunLight = _sun->findComponent<LightComponent>();
        if (sunLight) {
            sunLight->setShadowBias(0.2f);
            sunLight->setShadowNormalBias(0.05f);
            sunLight->setShadowDistance(120.0f);
            sunLight->setNumCascades(4);
            sunLight->setShadowResolution(2048);
        }

        auto* camera = createCamera(Vector3(-24.51f, 4.59f, 0.98f));
        _cameraComp = camera->findComponent<CameraComponent>();
        if (!_cameraComp || !_cameraComp->camera()) {
            return false;
        }
        _cameraComp->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX, LAYERID_WATER, LAYERID_UI});
        _cameraComp->camera()->setFov(60.0f);
        _cameraComp->camera()->setFarClip(3000.0f);
        if (auto* controls = addOrbitControls(camera, Vector3(4.6f, 2.11f, -5.85f))) {
            controls->setPitchRange(Vector2(-89.0f, 89.0f));
            controls->setZoomRange(Vector2(2.0f, 150.0f));
            controls->storeResetState();
        }

        // HDR rendering with bloom; the threshold keeps it to the sun and the brightest sky.
        _cameraComp->setToneMapping(TONEMAP_NEUTRAL);
        // The scene depth, for the water's depth effects, from the first frame.
        _cameraComp->requestSceneDepthMap(true);
        auto rendering = _cameraComp->rendering();
        rendering.bloomThreshold = 5.0f;
        rendering.bloomBlurLevel = 8;
        _cameraComp->setRendering(rendering);

        // The dynamic sky, lighting the scene and driving the sun.
        _sky = std::make_shared<DynamicSkyEffect>();
        _sky->setLight(sunLight);
        _sky->moonIntensity = 0.4f;
        _sky->cloudType = CloudType::Volumetric;
        _sky->cloudSize = 9000.0f;
        _sky->cloudHeight = 1800.0f;
        // The time of day animates, so the lighting blends between its updates instead of
        // stepping.
        _sky->lightingInterpolation = true;
        _cameraComp->addFrameEffect(_sky);

        // The auto exposure, adapting from the day to the night.
        _autoExposure = std::make_shared<AutoExposureEffect>();
        _cameraComp->addFrameEffect(_autoExposure);

        // The water: a tessellated circle, so the waves can displace it.
        _waterMesh = createMeshFromGeometry(device(), createCircleGeometry(1200.0f, 180, 150, 2.6f));
        _waterPlaceholder = std::make_shared<StandardMaterial>();
        auto* water = new Entity();
        water->setName("Water");
        water->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(water->addComponent<RenderComponent>())) {
            render->setLayers({LAYERID_WATER});
            render->setCastShadows(false);
            auto meshInstance = std::make_unique<MeshInstance>(_waterMesh.get(), _waterPlaceholder.get(), water);
            meshInstance->setCastShadow(false);
            render->addMeshInstance(std::move(meshInstance));
        }
        water->setLocalPosition(0.0f, -0.62f, 0.0f);
        root()->addChild(water);
        water->addComponent<ScriptComponent>();
        if (auto* script = water->script()->create<Water>()) {
            script->setCameraEntity(camera);
            script->setLightEntity(_sun);
            script->normalMap = _normal->resourceAs<Texture>();
            script->causticsMap = _caustics->resourceAs<Texture>();
            script->waves = true;
            script->shallowColor = Color(0.6f, 0.9f, 0.85f, 1.0f);
            script->deepColor = Color(0.03f, 0.16f, 0.24f, 1.0f);
            script->depthFade = 3.0f;
        }

        if (const char* hour = std::getenv("VISUTWIN_SKY_HOUR")) {
            _hour = static_cast<float>(std::atof(hour));
            _animate = false;
        }
        if (const char* bloom = std::getenv("VISUTWIN_SKY_BLOOM"); bloom && std::atoi(bloom) == 0) {
            _bloom = false;
        }
        if (const char* autoExposure = std::getenv("VISUTWIN_SKY_AUTO_EXPOSURE"); autoExposure && std::atoi(autoExposure) == 0) {
            _autoExposure->setEnabled(false);
        }
        _elevationCurve = Curve({0.0f, -60.0f, 6.0f, 0.0f, 12.0f, 60.0f, 18.0f, 0.0f, 24.0f, -90.0f});
        _elevationCurve.type = CURVE_SMOOTHSTEP;
        applySettings();

        spdlog::info("Controls: Space time, [ ] hour, C cloud type, -/= coverage, Q quality, "
                     "R resolution, B bloom, E auto exposure, T TAA");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_sky) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_SPACE: _animate = !_animate; break;
        case SDLK_LEFTBRACKET: _hour = std::fmod(_hour + 23.5f, 24.0f); break;
        case SDLK_RIGHTBRACKET: _hour = std::fmod(_hour + 0.5f, 24.0f); break;
        case SDLK_C:
            _cloudType = _cloudType == CloudType::None ? CloudType::Flat
                : (_cloudType == CloudType::Flat ? CloudType::Volumetric : CloudType::None);
            break;
        case SDLK_MINUS: _coverage = std::max(0.0f, _coverage - 0.05f); break;
        case SDLK_EQUALS: _coverage = std::min(1.0f, _coverage + 0.05f); break;
        case SDLK_Q: _quality = _quality < 0.25f ? 0.5f : (_quality < 0.75f ? 1.0f : 0.0f); break;
        case SDLK_R: _resolution = _resolution < 0.375f ? 0.5f : (_resolution < 0.75f ? 1.0f : 0.25f); break;
        case SDLK_B: _bloom = !_bloom; break;
        case SDLK_E: _autoExposure->setEnabled(!_autoExposure->enabled()); break;
        case SDLK_T: _taa = !_taa; break;
        default: return false;
        }
        applySettings();
        spdlog::info("hour {:.1f} ({}), clouds {}, coverage {:.2f}, quality {:.2f}, resolution {:.2f}, bloom {}, "
                     "auto exposure {}, taa {}", _hour, _animate ? "running" : "stopped",
            _cloudType == CloudType::None ? "none" : (_cloudType == CloudType::Flat ? "flat" : "volumetric"),
            _coverage, _quality, _resolution, _bloom ? "on" : "off",
            _autoExposure->enabled() ? "on" : "off", _taa ? "on" : "off");
        return true;
    }

    void update(const float dt) override
    {
        // The time of day, looping from 20:00 back to 05:00 while it runs, skipping most of the night.
        if (_animate) {
            _hour += dt * 0.25f;
            if (_hour >= 20.0f) {
                _hour -= 15.0f;
            }
        }
        // The sun, rising in the east and setting in the west.
        const float elevation = _elevationCurve.value(_hour);
        _sky->elevation = elevation;
        _sky->azimuth = (_hour / 24.0f) * 360.0f;

        // The underwater fog dims with the daylight, with a small floor for the moonlit night.
        const float t = std::clamp((elevation + 2.0f) / 14.0f, 0.0f, 1.0f);
        const float day = 0.05f + 0.95f * t * t * (3.0f - 2.0f * t);
        scene()->setFogColor(Color(0.165f * day, 0.549f * day, 0.596f * day, 1.0f));
    }

private:
    void applySettings()
    {
        _sky->cloudType = _cloudType;
        _sky->cloudCoverage = _coverage;
        _sky->cloudQuality = _quality;
        _sky->cloudResolution = _resolution;
        _sky->windSpeed = 30.0f;
        _sky->windDirection = 45.0f;
        // The clouds lighting the scene follow the drifting clouds.
        _sky->lightingCloudThreshold = 0.005f;
        _autoExposure->compensation = 0.5f;

        auto rendering = _cameraComp->rendering();
        rendering.bloomIntensity = _bloom ? 0.02f : 0.0f;
        _cameraComp->setRendering(rendering);
        auto taa = _cameraComp->taa();
        taa.enabled = _taa;
        _cameraComp->setTaa(taa);
    }

    std::unique_ptr<Asset> _island;
    std::unique_ptr<Asset> _normal;
    std::unique_ptr<Asset> _caustics;
    std::shared_ptr<Mesh> _waterMesh;
    std::shared_ptr<StandardMaterial> _waterPlaceholder;
    Entity* _sun = nullptr;
    CameraComponent* _cameraComp = nullptr;
    std::shared_ptr<DynamicSkyEffect> _sky;
    std::shared_ptr<AutoExposureEffect> _autoExposure;
    Curve _elevationCurve{std::vector<float>{}};

    float _hour = 5.5f;
    bool _animate = true;
    CloudType _cloudType = CloudType::Volumetric;
    float _coverage = 0.42f;
    float _quality = 0.5f;
    float _resolution = 1.0f;
    bool _bloom = true;
    bool _taa = false;
};

VISUTWIN_EXAMPLE_MAIN(DynamicSkyExample)
