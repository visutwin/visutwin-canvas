// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.07.2025.
//
// Orbit-camera reference scene: the statue under the helipad env atlas, with
// keyboard toggles for the normal-map debug view and the TAA settings.
//
#include <algorithm>
#include <memory>

#include <core/shape/boundingBox.h>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"

using namespace visutwin::canvas;

constexpr int LAYER_IMMEDIATE_EXAMPLE = 5;

class OrbitExample final: public ExampleApp
{
public:
    OrbitExample(): ExampleApp({.title = "Visualization Engine"}) {}

protected:
    bool create() override
    {
        spdlog::info("*** Visualization Engine Started *** ");

        scene()->setAmbientLight(0.4f, 0.4f, 0.4f);
        scene()->setDebugNormalMapsEnabled(false);
        scene()->setExposure(1.0f);

        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(0.4f);

        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        const auto helipadResource = _helipad->resource();
        if (!helipadResource) {
            spdlog::error("Failed to load helipad texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));

        auto* light = new Entity();
        light->setEngine(engine());
        light->addComponent<LightComponent>();
        light->setLocalEulerAngles(45, 30, 0);
        root()->addChild(light);

        _statue = std::make_unique<Asset>(
            "statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        const auto statueResource = _statue->resource();
        if (!statueResource) {
            spdlog::error("Failed to load statue model");
            return false;
        }
        auto* statueEntity = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
        statueEntity->setLocalPosition(0, -0.5, 0);
        root()->addChild(statueEntity);

        const auto bbox = entityBounds(statueEntity);
        _focusPoint = bbox.center();
        _sceneRadius = std::max(bbox.halfExtents().length(), 1.0f);
        const auto start = Vector3(0.0f, 20.0f, 30.0f);

        auto* camera = createCamera(start);
        _cameraComponentA = camera->findComponent<CameraComponent>();

        _controls = addOrbitControls(camera, _focusPoint);
        const float sceneSize = _sceneRadius;
        _controls->setMoveSpeed(2 * sceneSize);
        _controls->setMoveFastSpeed(4 * sceneSize);
        _controls->setMoveSlowSpeed(sceneSize);
        _controls->setOrbitDistance(std::max(_sceneRadius * 4.0f, 12.0f));
        _controls->storeResetState();

        if (_cameraComponentA) {
            auto taa = _cameraComponentA->taa();
            taa.enabled = false;
            taa.highQuality = true;
            taa.jitter = 0.7f;
            _cameraComponentA->setTaa(taa);
        }

        auto* cameraB = createCamera(start);
        if (auto* cameraComponentB = cameraB->findComponent<CameraComponent>();
            cameraComponentB && cameraComponentB->camera()) {
            cameraComponentB->setLayers({LAYER_IMMEDIATE_EXAMPLE});
            cameraComponentB->camera()->setClearColorBuffer(false);
            cameraComponentB->camera()->setClearDepthBuffer(false);
            cameraComponentB->camera()->setClearStencilBuffer(false);
        }

        spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
        spdlog::info("Render controls: N normal-map debug, T toggle TAA, Y toggle TAA quality, [ and ] adjust TAA jitter");
        logTaaState("init");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }

        switch (event.key.key) {
        case SDLK_N: {
            const bool enabled = !scene()->debugNormalMapsEnabled();
            scene()->setDebugNormalMapsEnabled(enabled);
            spdlog::info("Normal map debug toggle: {}", enabled ? "ON" : "OFF");
            return true;
        }
        case SDLK_T:
            if (_cameraComponentA) {
                auto taa = _cameraComponentA->taa();
                taa.enabled = !taa.enabled;
                _cameraComponentA->setTaa(taa);
                logTaaState("toggle");
            }
            return true;
        case SDLK_Y:
            if (_cameraComponentA) {
                auto taa = _cameraComponentA->taa();
                taa.highQuality = !taa.highQuality;
                _cameraComponentA->setTaa(taa);
                logTaaState("quality");
            }
            return true;
        case SDLK_LEFTBRACKET:
            if (_cameraComponentA) {
                auto taa = _cameraComponentA->taa();
                taa.jitter = std::max(0.0f, taa.jitter - 0.1f);
                _cameraComponentA->setTaa(taa);
                logTaaState("jitter");
            }
            return true;
        case SDLK_RIGHTBRACKET:
            if (_cameraComponentA) {
                auto taa = _cameraComponentA->taa();
                taa.jitter = std::min(2.0f, taa.jitter + 0.1f);
                _cameraComponentA->setTaa(taa);
                logTaaState("jitter");
            }
            return true;
        case SDLK_F:
            if (_controls) {
                _controls->focus(_focusPoint, std::max(_sceneRadius * 2.0f, 6.0f));
            }
            return true;
        default:
            return false;
        }
    }

    void destroy() override
    {
        spdlog::info("*** Visualization Engine Finished *** ");
    }

private:

    void logTaaState(const char* reason) const
    {
        if (!_cameraComponentA) {
            return;
        }
        const auto& taa = _cameraComponentA->taa();
        spdlog::info("TAA {}: enabled={}, highQuality={}, jitter={:.2f}",
            reason,
            taa.enabled ? "ON" : "OFF",
            taa.highQuality ? "ON" : "OFF",
            taa.jitter);
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _statue;

    CameraComponent* _cameraComponentA = nullptr;
    CameraControls* _controls = nullptr;
    Vector3 _focusPoint;
    float _sceneRadius = 1.0f;
};

VISUTWIN_EXAMPLE_MAIN(OrbitExample)
