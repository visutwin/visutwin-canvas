// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Port of upstream camera-frame/taa.
//
// The PBR house (scaled 100x) under the table-mountain env atlas (skybox mip 0,
// exposure 2.5), a shadow-casting directional light, and a cube orbiting the
// house at radius 130 while it spins. The camera frame runs TAA (jitter 1),
// ACES tone mapping, bloom 0.02 and sharpness 1; an orbit camera frames the
// house on start.
//
// The mode compares the full resolution with a lower one, set by the resolution, at which the
// image is either upscaled with bilinear filtering or, with TAA upscaling, accumulated from the
// jittered frames at the full resolution, sharpened or not: full, scaled, scaled + sharpen,
// temporal, temporal + sharpen. The scaled modes remember their resolution (0.5 to begin with).
//
// Keys stand in for upstream's controls panel, over the same parameters:
//   M      mode, cycled
//   - / =  resolution -/+ 0.05, 0.25..1 (lowering it at the full resolution selects scaled)
//   T      TAA enabled (like upstream, turning it on sets sharpness 1, off 0)
//   [ / ]  TAA jitter -/+ 0.05, 0..1
//   S      sharpness, cycled 0 / 0.25 / 0.5 / 0.75 / 1 (applied by the sharpening modes)
//   B      bloom on (0.02) / off
// VISUTWIN_TAA_MODE=full|scaled|sharpen|temporal|temporalSharpen starts in that mode, for a
// capture.
//
// DEVIATIONS:
// - upstream's orbitCamera script (inertia 0.2, distanceMax 400, frameOnStart)
//   is replaced by the examples' CameraControls in orbit mode. The start pose is
//   computed the way frameOnStart does it: pivot at the house's AABB centre, the
//   viewing direction from the authored camera position (0, 40, -220), distance
//   1.5 * max half-extent / sin(fov / 2), clamped to 400. There is no inertia.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string_view>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

namespace
{
    enum class Mode { Full, Scaled, Sharpen, Temporal, TemporalSharpen };
    constexpr std::array<std::string_view, 5> kModeNames = {"full", "scaled", "sharpen", "temporal",
        "temporalSharpen"};
}

class TaaExample final: public ExampleApp
{
public:
    TaaExample(): ExampleApp({.title = "TAA Example", .antialias = false}) {}

protected:
    bool create() override
    {
        _envAtlas = std::make_unique<Asset>(
            "env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/table-mountain-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _house = std::make_unique<Asset>(
            "house", AssetType::CONTAINER, assetPath("models/pbr-house.glb"));
        _cube = std::make_unique<Asset>(
            "cube", AssetType::CONTAINER, assetPath("models/playcanvas-cube.glb"));

        // Setup skydome with low intensity
        if (Texture* envAtlasTexture = _envAtlas->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlasTexture);
        } else {
            spdlog::warn("Failed to load environment atlas — continuing without IBL");
        }
        scene()->setSkyboxMip(0);
        scene()->setExposure(2.5f);

        // Create an instance of the house and add it to the scene
        ContainerResource* houseContainer = _house->resourceAs<ContainerResource>();
        if (!houseContainer) {
            spdlog::error("Failed to load house model");
            return false;
        }
        auto* houseEntity = houseContainer->instantiateRenderEntity();
        houseEntity->setLocalScale(100, 100, 100);
        root()->addChild(houseEntity);

        // Frame the house the way frameOnStart does.
        constexpr float fov = 80.0f;
        const auto houseBbox = entityBounds(houseEntity);
        const Vector3 pivot = houseBbox.center();
        const Vector3 halfExtents = houseBbox.halfExtents();
        const float radius = std::max({halfExtents.getX(), halfExtents.getY(), halfExtents.getZ()});
        const float distance = std::min(radius * 1.5f / std::sin(0.5f * fov * DEG_TO_RAD), 400.0f);
        const Vector3 authoredPosition(0.0f, 40.0f, -220.0f);
        const Vector3 cameraPosition = pivot + (authoredPosition - pivot).normalized() * distance;

        // Create an Entity with a camera component
        auto* camera = createCamera(cameraPosition);
        _cameraComp = camera->findComponent<CameraComponent>();
        if (_cameraComp && _cameraComp->camera()) {
            _cameraComp->camera()->setNearClip(10.0f);
            _cameraComp->camera()->setFarClip(600.0f);
            _cameraComp->camera()->setFov(fov);
        }

        auto* controls = addOrbitControls(camera, pivot);
        if (controls) {
            controls->setZoomRange(Vector2(0.0f, 400.0f));
            controls->storeResetState();
        }

        // Add a shadow casting directional light
        auto* light = createDirectionalLight(Vector3(40.0f, 10.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowResolution(4096);
            lightComp->setShadowDistance(600.0f);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
        }

        if (ContainerResource* cubeContainer = _cube->resourceAs<ContainerResource>()) {
            _cubeEntity = cubeContainer->instantiateRenderEntity();
            _cubeEntity->setLocalScale(30, 30, 30);
            root()->addChild(_cubeEntity);
        }

        // Camera frame: ACES, then the initial control values (full resolution, bloom on,
        // sharpness 1, TAA on with jitter 1).
        if (!_cameraComp) {
            return false;
        }
        _cameraComp->setToneMapping(TONEMAP_ACES);
        if (const char* mode = std::getenv("VISUTWIN_TAA_MODE")) {
            for (size_t i = 0; i < kModeNames.size(); ++i) {
                if (kModeNames[i] == mode) {
                    setMode(static_cast<Mode>(i));
                }
            }
        }
        applySettings();

        spdlog::info("Controls: M mode, -/= resolution, T TAA, [ ] jitter, S sharpness, B bloom");
        logState("init");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }

        switch (event.key.key) {
        case SDLK_M:
            setMode(static_cast<Mode>((static_cast<int>(_mode) + 1) % static_cast<int>(kModeNames.size())));
            break;
        case SDLK_MINUS:
            setScale(_scale - 0.05f);
            break;
        case SDLK_EQUALS:
            setScale(_scale + 0.05f);
            break;
        case SDLK_T:
            _taaEnabled = !_taaEnabled;
            // TAA has been flipped, setup sharpening appropriately.
            _sharpness = _taaEnabled ? 1.0f : 0.0f;
            break;
        case SDLK_LEFTBRACKET:
            _jitter = std::max(0.0f, _jitter - 0.05f);
            break;
        case SDLK_RIGHTBRACKET:
            _jitter = std::min(1.0f, _jitter + 0.05f);
            break;
        case SDLK_S:
            if (_sharpness < 0.125f) _sharpness = 0.25f;
            else if (_sharpness < 0.375f) _sharpness = 0.5f;
            else if (_sharpness < 0.625f) _sharpness = 0.75f;
            else if (_sharpness < 0.875f) _sharpness = 1.0f;
            else _sharpness = 0.0f;
            break;
        case SDLK_B:
            _bloom = !_bloom;
            break;
        default:
            return false;
        }

        applySettings();
        logState("changed");
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        if (_cubeEntity) {
            _cubeEntity->setLocalPosition(130.0f * std::sin(_time), 0.0f, 130.0f * std::cos(_time));
            _cubeEntity->rotate(50.0f * dt, 20.0f * dt, 30.0f * dt);
        }
    }

private:
    // The resolution follows the mode: 1 at the full one, the remembered one in the scaled modes.
    void setMode(const Mode mode)
    {
        _mode = mode;
        _scale = mode == Mode::Full ? 1.0f : _scaledResolution;
    }

    // Lowering the resolution while at the full one switches to the scaled mode.
    void setScale(const float scale)
    {
        _scale = std::clamp(std::round(scale * 100.0f) / 100.0f, 0.25f, 1.0f);
        if (_scale < 1.0f) {
            _scaledResolution = _scale;
            if (_mode == Mode::Full) {
                _mode = Mode::Scaled;
            }
        }
    }

    void applySettings()
    {
        const bool sharpen = _mode == Mode::Full || _mode == Mode::Sharpen || _mode == Mode::TemporalSharpen;

        auto taa = _cameraComp->taa();
        taa.enabled = _taaEnabled;
        taa.jitter = _jitter;
        taa.upscale = _mode == Mode::Temporal || _mode == Mode::TemporalSharpen;
        _cameraComp->setTaa(taa);

        auto rendering = _cameraComp->rendering();
        rendering.bloomIntensity = _bloom ? 0.02f : 0.0f;
        rendering.renderTargetScale = _scale;
        rendering.sharpness = sharpen ? _sharpness : 0.0f;
        _cameraComp->setRendering(rendering);
    }

    void logState(const char* reason) const
    {
        spdlog::info("{}: mode={}, resolution={:.2f}, taa={}, jitter={:.2f}, sharpness={:.2f}, bloom={}",
            reason, kModeNames[static_cast<size_t>(_mode)], _scale, _taaEnabled ? "ON" : "OFF", _jitter,
            _cameraComp->rendering().sharpness, _bloom ? "on" : "off");
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _house;
    std::unique_ptr<Asset> _cube;

    CameraComponent* _cameraComp = nullptr;
    Entity* _cubeEntity = nullptr;

    Mode _mode = Mode::Full;
    float _scale = 1.0f;
    float _scaledResolution = 0.5f;
    float _sharpness = 1.0f;
    float _jitter = 1.0f;
    bool _taaEnabled = true;
    bool _bloom = true;

    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(TaaExample)
