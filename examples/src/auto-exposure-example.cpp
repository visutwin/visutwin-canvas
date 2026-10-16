// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.10.2026
//
// Port of upstream camera-frame/auto-exposure.
//
// The De Dust 2 level (its lighting baked in, scaled 2x) under the morning env atlas (skybox
// mip 0, rotated 10 degrees, scene exposure 0.4); the camera frame renders with 4x MSAA, bloom
// 0.01, a vignette (0.4, inner 0.6, outer 1.2) and ACES2, and an AutoExposureEffect adapts the
// exposure as the camera moves between the sunlit courtyards and the dark tunnels, faster to
// the light than to the dark. Its settings are upstream's start values: compensation 0.2, EV
// -6.9..4, speeds 3 / 1, metering 0.5..0.95 of the screen, centre weight 0.45.
//
// Keys stand in for upstream's controls panel:
//   WASD / mouse  fly (see DEVIATIONS)
//   E      auto exposure on / off
//   T      tone mapping, cycled: ACES2, ACES, Neutral, Linear, None
//   [ / ]  compensation -/+ 0.1 EV
//   H      the histogram debug view on / off
// The exposure relative to the scene's is logged twice a second, as upstream's readout shows it.
// VISUTWIN_AE_VIEW=x,y,z,yaw[,pitch] puts the camera there, for a capture, and
// VISUTWIN_AE_HISTOGRAM=1 starts with the debug view.
//
// DEVIATIONS:
// - upstream walks a physics character controller (FirstPersonController with a capsule on the
//   level's mesh collision, jumping); this port flies the examples' CameraControls through the
//   level instead, with no collision.
// - upstream boosts the clipped sun of the sky 50x (skyboxHighlightMultiplier), which this
//   engine does not have: the sun is no brighter than the sky's brightest texels, so the
//   sunlit views meter a little darker than upstream's.
//
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/graphics/autoExposureEffect.h"

using namespace visutwin::canvas;

namespace
{
    constexpr std::array<int, 5> kToneMappings = {TONEMAP_ACES2, TONEMAP_ACES, TONEMAP_NEUTRAL, TONEMAP_LINEAR,
        TONEMAP_NONE};
    constexpr std::array<const char*, 5> kToneMappingNames = {"ACES2", "ACES", "Neutral", "Linear", "None"};
}

class AutoExposureExample final: public ExampleApp
{
public:
    // The scene is rendered to an antialiased texture, so the back buffer needs none.
    AutoExposureExample(): ExampleApp({.title = "Auto Exposure Example", .antialias = false}) {}

protected:
    bool create() override
    {
        _map = std::make_unique<Asset>("map", AssetType::CONTAINER, assetPath("models/fps-map.glb"));
        _morning = std::make_unique<Asset>("morning-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/morning-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});

        // A sunny sky.
        scene()->setSkyboxMip(0);
        scene()->setExposure(0.4f);
        if (Texture* envAtlas = _morning->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        }
        scene()->setSkyboxRotation(Quaternion::fromEulerAngles(0.0f, 10.0f, 0.0f));

        // The level, with its lighting baked in.
        ContainerResource* mapContainer = _map->resourceAs<ContainerResource>();
        if (!mapContainer) {
            spdlog::error("Failed to load the level");
            return false;
        }
        auto* map = mapContainer->instantiateRenderEntity();
        map->setLocalScale(2.0f, 2.0f, 2.0f);
        map->setLocalEulerAngles(-90.0f, 0.0f, 0.0f);
        root()->addChild(map);

        // The camera at the character's start (5, 2, 10), its eyes 0.5 above it.
        Vector3 position(5.0f, 2.5f, 10.0f);
        float yaw = 0.0f;
        float pitch = 0.0f;
        if (const char* view = std::getenv("VISUTWIN_AE_VIEW")) {
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (std::sscanf(view, "%f,%f,%f,%f,%f", &x, &y, &z, &yaw, &pitch) >= 4) {
                position = Vector3(x, y, z);
            }
        }
        auto* camera = createCamera(position, Vector3(pitch, yaw, 0.0f));
        _cameraComp = camera->findComponent<CameraComponent>();
        if (!_cameraComp || !_cameraComp->camera()) {
            return false;
        }
        _cameraComp->camera()->setFarClip(100.0f);
        _cameraComp->camera()->setFov(90.0f);

        if (!camera->script()) {
            camera->addComponent<ScriptComponent>();
        }
        if (auto* controls = camera->script()->create<CameraControls>()) {
            controls->setEnableOrbit(false);
            controls->setEnableFly(true);
            controls->setEnablePan(false);
            controls->setMoveSpeed(6.0f);
        }

        // The camera frame, rendering in HDR with a bloom on the bright sky.
        auto rendering = _cameraComp->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.01f;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.4f;
        rendering.vignetteInner = 0.6f;
        rendering.vignetteOuter = 1.2f;
        if (const char* histogram = std::getenv("VISUTWIN_AE_HISTOGRAM"); histogram && std::atoi(histogram) != 0) {
            rendering.composeDebug = "autoexposure";
        }
        _cameraComp->setRendering(rendering);

        // The auto exposure, not part of the camera frame by default.
        _autoExposure = std::make_shared<AutoExposureEffect>();
        _autoExposure->compensation = 0.2f;
        _autoExposure->minEv = -6.9f;
        _autoExposure->maxEv = 4.0f;
        _autoExposure->speedDarkToLight = 3.0f;
        _autoExposure->speedLightToDark = 1.0f;
        _autoExposure->lowPercent = 0.5f;
        _autoExposure->highPercent = 0.95f;
        _autoExposure->centerWeight = 0.45f;
        _cameraComp->addFrameEffect(_autoExposure);

        applyToneMapping();
        spdlog::info("Controls: WASD/mouse fly, E auto exposure, T tone mapping, [ ] compensation, H histogram");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp || !_autoExposure) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_E:
            _autoExposure->setEnabled(!_autoExposure->enabled());
            break;
        case SDLK_T:
            _toneMapping = (_toneMapping + 1) % static_cast<int>(kToneMappings.size());
            applyToneMapping();
            break;
        case SDLK_LEFTBRACKET:
            _autoExposure->compensation -= 0.1f;
            break;
        case SDLK_RIGHTBRACKET:
            _autoExposure->compensation += 0.1f;
            break;
        case SDLK_H: {
            auto rendering = _cameraComp->rendering();
            rendering.composeDebug = rendering.composeDebug.empty() ? "autoexposure" : "";
            _cameraComp->setRendering(rendering);
            break;
        }
        default:
            return false;
        }
        spdlog::info("auto exposure {}, tone mapping {}, compensation {:+.1f} EV, histogram {}",
            _autoExposure->enabled() ? "on" : "off", kToneMappingNames[static_cast<size_t>(_toneMapping)],
            _autoExposure->compensation, _cameraComp->rendering().composeDebug.empty() ? "off" : "on");
        return true;
    }

    void update(const float dt) override
    {
        // The exposure the camera renders with, relative to the scene's.
        _readoutTime += dt;
        if (_readoutTime > 0.5f && _autoExposure) {
            _readoutTime = 0.0f;
            if (_autoExposure->active()) {
                spdlog::info("exposure {:+.2f} EV", std::log2(_autoExposure->exposure() / scene()->exposure()));
            }
        }
    }

private:
    void applyToneMapping()
    {
        _cameraComp->setToneMapping(kToneMappings[static_cast<size_t>(_toneMapping)]);
    }

    std::unique_ptr<Asset> _map;
    std::unique_ptr<Asset> _morning;
    std::shared_ptr<AutoExposureEffect> _autoExposure;
    CameraComponent* _cameraComp = nullptr;
    int _toneMapping = 0;
    float _readoutTime = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(AutoExposureExample)
