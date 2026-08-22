// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// TAA reference scene: the PBR house under the table-mountain env atlas with a
// shadow-casting directional light and an orbiting cube, plus keyboard controls
// for the TAA and camera-frame rendering settings.
//
#include <algorithm>
#include <cmath>
#include <memory>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class TaaExample final: public ExampleApp
{
public:
    TaaExample(): ExampleApp({.title = "TAA Example"}) {}

protected:
    bool create() override
    {
        spdlog::info("*** TAA Example Started ***");

        // setup skydome
        scene()->setSkyboxMip(0);
        scene()->setExposure(2.5f);
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // add shadow casting directional light
        auto* light = createDirectionalLight(Vector3(40.0f, 10.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowResolution(4096);
            lightComp->setShadowDistance(600.0f);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
        }

        // ── Load resources synchronously ──────────────────────────────────────
        // Assets matching upstream TAA example
        _envAtlas = std::make_unique<Asset>(
            "table-mountain-env-atlas",
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

        if (const auto envAtlasResource = _envAtlas->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
        } else {
            spdlog::warn("Failed to load environment atlas — continuing without IBL");
        }

        const auto houseResource = _house->resource();
        if (!houseResource) {
            spdlog::error("Failed to load house model");
            return false;
        }
        auto* houseEntity = std::get<ContainerResource*>(*houseResource)->instantiateRenderEntity();
        houseEntity->setLocalScale(100, 100, 100);
        root()->addChild(houseEntity);

        if (const auto cubeResource = _cube->resource()) {
            _cubeEntity = std::get<ContainerResource*>(*cubeResource)->instantiateRenderEntity();
            _cubeEntity->setLocalScale(30, 30, 30);
            root()->addChild(_cubeEntity);
        }

        // Auto-frame the house model
        const auto houseBbox = entityBounds(houseEntity);
        _focusPoint = houseBbox.center();
        const float sceneRadius = std::max(houseBbox.halfExtents().length(), 1.0f);
        _orbitDistance = std::max(sceneRadius * 2.0f, 220.0f);

        // create camera entity
        auto* camera = createCamera(_focusPoint + Vector3(0.0f, sceneRadius * 0.3f, _orbitDistance));
        _cameraComp = camera->findComponent<CameraComponent>();

        if (_cameraComp && _cameraComp->camera()) {
            _cameraComp->camera()->setNearClip(std::max(1.0f, sceneRadius * 0.01f));
            _cameraComp->camera()->setFarClip(std::max(600.0f, sceneRadius * 6.0f));
            _cameraComp->camera()->setFov(80.0f);
        }

        // TAA enabled by default, jitter=1.0
        if (_cameraComp) {
            auto taa = _cameraComp->taa();
            taa.enabled = true;
            taa.highQuality = true;
            taa.jitter = 1.0f;
            _cameraComp->setTaa(taa);

            auto rendering = _cameraComp->rendering();
            rendering.bloomIntensity = 0.02f;
            rendering.sharpness = 0.5f;
            rendering.renderTargetScale = 1.0f;
            rendering.toneMapping = TONEMAP_ACES;
            _cameraComp->setRendering(rendering);
        }

        _controls = addOrbitControls(camera, _focusPoint);
        _controls->setAutoFarClip(true);
        _controls->setMoveSpeed(2 * sceneRadius);
        _controls->setMoveFastSpeed(4 * sceneRadius);
        _controls->setMoveSlowSpeed(sceneRadius);
        _controls->setOrbitDistance(_orbitDistance);
        _controls->storeResetState();

        spdlog::info("House AABB center=({:.1f},{:.1f},{:.1f}), radius={:.1f}, orbit={:.1f}",
            _focusPoint.getX(), _focusPoint.getY(), _focusPoint.getZ(),
            sceneRadius, _orbitDistance);

        spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
        spdlog::info("TAA controls: T toggle TAA, Y toggle quality, [ ] adjust jitter");
        spdlog::info("Render controls: B toggle bloom, S cycle sharpness, M cycle tonemap, -/= adjust scale, N normal debug");
        logTaaState("init");
        logRenderingState("init");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }

        switch (event.key.key) {
        // TAA controls
        case SDLK_T: {
            auto taa = _cameraComp->taa();
            taa.enabled = !taa.enabled;
            _cameraComp->setTaa(taa);
            logTaaState("toggle");
            return true;
        }
        case SDLK_Y: {
            auto taa = _cameraComp->taa();
            taa.highQuality = !taa.highQuality;
            _cameraComp->setTaa(taa);
            logTaaState("quality");
            return true;
        }
        case SDLK_LEFTBRACKET: {
            auto taa = _cameraComp->taa();
            taa.jitter = std::max(0.0f, taa.jitter - 0.05f);
            _cameraComp->setTaa(taa);
            logTaaState("jitter-");
            return true;
        }
        case SDLK_RIGHTBRACKET: {
            auto taa = _cameraComp->taa();
            taa.jitter = std::min(1.0f, taa.jitter + 0.05f);
            _cameraComp->setTaa(taa);
            logTaaState("jitter+");
            return true;
        }

        // Rendering controls
        case SDLK_B: {
            auto rendering = _cameraComp->rendering();
            rendering.bloomIntensity = rendering.bloomIntensity > 0.0f ? 0.0f : 0.02f;
            _cameraComp->setRendering(rendering);
            logRenderingState("bloom");
            return true;
        }
        case SDLK_S: {
            // Cycle sharpness: 0.0 -> 0.25 -> 0.5 -> 0.75 -> 1.0 -> 0.0
            auto rendering = _cameraComp->rendering();
            if (rendering.sharpness < 0.125f) rendering.sharpness = 0.25f;
            else if (rendering.sharpness < 0.375f) rendering.sharpness = 0.5f;
            else if (rendering.sharpness < 0.625f) rendering.sharpness = 0.75f;
            else if (rendering.sharpness < 0.875f) rendering.sharpness = 1.0f;
            else rendering.sharpness = 0.0f;
            _cameraComp->setRendering(rendering);
            logRenderingState("sharpness");
            return true;
        }
        case SDLK_M: {
            // Cycle tonemapping: LINEAR -> ACES -> NONE -> LINEAR
            auto rendering = _cameraComp->rendering();
            if (rendering.toneMapping == TONEMAP_LINEAR) rendering.toneMapping = TONEMAP_ACES;
            else if (rendering.toneMapping == TONEMAP_ACES) rendering.toneMapping = TONEMAP_NONE;
            else rendering.toneMapping = TONEMAP_LINEAR;
            _cameraComp->setRendering(rendering);
            // Also update scene tonemapping
            scene()->setToneMapping(rendering.toneMapping);
            logRenderingState("tonemap");
            return true;
        }
        case SDLK_MINUS: {
            auto rendering = _cameraComp->rendering();
            rendering.renderTargetScale = std::max(0.5f, rendering.renderTargetScale - 0.1f);
            _cameraComp->setRendering(rendering);
            logRenderingState("scale-");
            return true;
        }
        case SDLK_EQUALS: {
            auto rendering = _cameraComp->rendering();
            rendering.renderTargetScale = std::min(1.0f, rendering.renderTargetScale + 0.1f);
            _cameraComp->setRendering(rendering);
            logRenderingState("scale+");
            return true;
        }

        // Scene controls
        case SDLK_N: {
            const bool enabled = !scene()->debugNormalMapsEnabled();
            scene()->setDebugNormalMapsEnabled(enabled);
            spdlog::info("Normal map debug toggle: {}", enabled ? "ON" : "OFF");
            return true;
        }
        case SDLK_F:
            if (_controls) {
                _controls->focus(_focusPoint, _orbitDistance);
            }
            return true;

        default:
            return false;
        }
    }

    void update(const float dt) override
    {
        // animate the cube — orbit + rotate
        _totalTime += dt;
        if (_cubeEntity) {
            _cubeEntity->setLocalPosition(
                130.0f * std::sin(_totalTime),
                0.0f,
                130.0f * std::cos(_totalTime)
            );
            _cubeRotX += 50.0f * dt;
            _cubeRotY += 20.0f * dt;
            _cubeRotZ += 30.0f * dt;
            _cubeEntity->setLocalEulerAngles(_cubeRotX, _cubeRotY, _cubeRotZ);
        }
    }

    void destroy() override
    {
        spdlog::info("*** TAA Example Finished ***");
    }

private:
    void logTaaState(const char* reason) const
    {
        if (!_cameraComp) {
            return;
        }
        const auto& taa = _cameraComp->taa();
        spdlog::info("TAA {}: enabled={}, highQuality={}, jitter={:.2f}",
            reason,
            taa.enabled ? "ON" : "OFF",
            taa.highQuality ? "ON" : "OFF",
            taa.jitter);
    }

    void logRenderingState(const char* reason) const
    {
        if (!_cameraComp) {
            return;
        }
        const auto& rendering = _cameraComp->rendering();
        spdlog::info("Rendering {}: bloom={:.3f}, sharpness={:.2f}, scale={:.1f}, tonemap={}",
            reason,
            rendering.bloomIntensity,
            rendering.sharpness,
            rendering.renderTargetScale,
            rendering.toneMapping == TONEMAP_ACES ? "ACES" :
            rendering.toneMapping == TONEMAP_NONE ? "NONE" : "LINEAR");
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _house;
    std::unique_ptr<Asset> _cube;

    CameraComponent* _cameraComp = nullptr;
    CameraControls* _controls = nullptr;
    Entity* _cubeEntity = nullptr;

    Vector3 _focusPoint;
    float _orbitDistance = 220.0f;
    float _totalTime = 0.0f;
    float _cubeRotX = 0.0f;
    float _cubeRotY = 0.0f;
    float _cubeRotZ = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(TaaExample)
