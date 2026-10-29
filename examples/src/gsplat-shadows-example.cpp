// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.10.2026
//
// Port of upstream gaussian-splatting/shadows.
//
// Gaussian splats casting shadows onto a shadow catcher. Two splat models cast as opaque
// footprints clipped at alpha 0.4 onto a 10 x 10 shadow-catcher plane, under the St Peter's
// Square night HDR shown as a projected dome (scale 50, centre (0, 0.05, 0)) and lighting the
// scene through its environment atlas, exposure 0.4. Up to six white directional lights
// (intensity 1, shadow bias 0.1, normal bias 0.05, distance 20, intensity 0.5, 2048 maps,
// PCF5) are pitched 55 degrees at azimuths 30, 210, 90, 270, 150 and 330, the first two on,
// and turn together at 20 degrees a second. The camera (fov 60, ACES, white clear) orbits the
// first model's bounds centre from (-3, 2, 4), out to 10.
//
// Keys stand in for upstream's control panel:
//   0-6    number of lights
//   T      next shadow type (PCF1, PCF3, PCF5, VSM16, PCSS)
//   [ / ]  shadow alpha clip -/+ 0.05
//
// DEVIATIONS:
// - upstream casts two copies of its `biker` capture; this uses the CC-BY-4.0 Tamiya DT-03
//   capture instead (see tamiya-dt03.txt), fitted to the bikers' places as the gsplat example
//   fits it: scale 0.148, raised to stand on the ground, the second turned 90 degrees from the
//   first as upstream's second biker is. The capture stands in its display crate, about 1.8
//   units wide here, so the second stands at x = 1.0 rather than 0.5, clear of the first.
// - Two directional lights per layer can be shadowed here: a third to sixth light lights the
//   models but casts nothing (the renderer says so once).
// - There are no 16-bit PCF maps or a 32-bit VSM: upstream's PCF5_16F default is PCF5_32F, and
//   the type key steps through the five types this engine has.
// - The alpha clip is each splat's own (GSplatComponent::setAlphaClip), where upstream sets it
//   scene-wide; both models take every change.
// - upstream's toggle for a custom splat vertex shader (animated splat centres) has no
//   counterpart: the splat program has no chunk to override.
// - upstream's control panel picks its splat renderer (CPU or GPU sort); this engine has the
//   one.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/gsplat/gsplatComponent.h"
#include "framework/components/gsplat/gsplatComponentSystem.h"
#include "framework/script/shadowCatcher.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/graphics/envLighting.h"
#include "scene/gsplat/gsplatResource.h"

using namespace visutwin::canvas;

namespace
{
    // Each light's fixed azimuth, ordered so that the enabled set stays well spread: light 1 is
    // opposite light 0, and all six are 60 degrees apart.
    constexpr std::array<float, 6> kLightAzimuths = {30.0f, 210.0f, 90.0f, 270.0f, 150.0f, 330.0f};
    constexpr std::array<ShadowType, 5> kShadowTypes = {ShadowType::SHADOW_PCF1_32F,
        ShadowType::SHADOW_PCF3_32F, ShadowType::SHADOW_PCF5_32F, ShadowType::SHADOW_VSM_16F,
        ShadowType::SHADOW_PCSS_32F};
    constexpr std::array<const char*, 5> kShadowTypeNames = {"PCF1", "PCF3", "PCF5", "VSM16", "PCSS"};

    // The car fitted to a biker's place (see the gsplat example): its scale, and the offset
    // that stands it on the ground at the biker's position.
    constexpr float kModelScale = 0.148f;
    const Vector3 kModelOffset(-0.049f, 0.443f, -0.031f);
}

class GsplatShadowsExample final : public ExampleApp
{
public:
    GsplatShadowsExample() : ExampleApp({.title = "Gaussian Splatting: Shadows", .antialias = false}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<GSplatComponentSystem>();
    }

    bool create() override
    {
        _hdrAsset = std::make_unique<Asset>("hdri", AssetType::TEXTURE, assetPath("hdri/st-peters-square.hdr"),
            AssetData{.mipmaps = false});
        auto* hdr = _hdrAsset->resourceAs<Texture>();
        auto model = GSplatResource::loadPly(assetPath("models/tamiya-dt03.compressed.ply"), device());
        if (!hdr || !model) {
            spdlog::error("gsplat-shadows: the HDR or the splat failed to load");
            return false;
        }

        // The HDR as a projected dome, and the environment lighting the scene
        _skybox.reset(EnvLighting::generateSkyboxCubemap(device().get(), hdr));
        _envAtlas.reset(EnvLighting::generateAtlas(device().get(), hdr));
        scene()->setSkybox(_skybox.get());
        scene()->setEnvAtlas(_envAtlas.get());
        scene()->setExposure(0.4f);
        scene()->setSkyType(SKYTYPE_DOME);
        scene()->sky()->node()->setLocalScale(Vector3(50.0f, 50.0f, 50.0f));
        scene()->sky()->node()->setLocalPosition(Vector3(0.0f, 0.0f, 0.0f));
        scene()->sky()->setCenter(Vector3(0.0f, 0.05f, 0.0f));

        // The two models, one splat resource between them, where upstream stands its bikers
        const auto addModel = [&](const char* name, const Vector3& position, const float yaw) {
            auto* entity = new Entity();
            entity->setName(name);
            entity->setEngine(engine());
            root()->addChild(entity);
            auto* gsplat = static_cast<GSplatComponent*>(entity->addComponent<GSplatComponent>());
            gsplat->setResource(model);
            gsplat->setCastShadows(true);
            gsplat->setAlphaClip(_alphaClip);
            entity->setLocalPosition(position + kModelOffset);
            entity->setLocalEulerAngles(180.0f, yaw, 0.0f);
            entity->setLocalScale(kModelScale, kModelScale, kModelScale);
            _models.push_back(gsplat);
            return entity;
        };
        Entity* first = addModel("model", Vector3(-1.5f, 0.05f, 0.0f), 0.0f);
        addModel("model2", Vector3(1.0f, 0.05f, 0.0f), -90.0f);

        // The camera, orbiting the first model
        Entity* camera = createCamera(Vector3(-3.0f, 2.0f, 4.0f));
        const Vector3 pivot = entityBounds(first).center();
        camera->lookAt(pivot);
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            cameraComponent->camera()->setFov(60.0f);
            cameraComponent->setToneMapping(TONEMAP_ACES);
        }
        if (CameraControls* controls = addOrbitControls(camera, pivot)) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(0.01f, 10.0f));
            controls->storeResetState();
        }

        // The shadow catcher
        auto* catcher = new Entity();
        catcher->setName("ShadowCatcher");
        catcher->setEngine(engine());
        catcher->addComponent<ScriptComponent>();
        catcher->script()->create<ShadowCatcher>()->setPlaneScale(10.0f);
        root()->addChild(catcher);

        // The lights, the first two on
        for (size_t i = 0; i < kLightAzimuths.size(); ++i) {
            Entity* light = createDirectionalLight(Vector3(55.0f, kLightAzimuths[i], 0.0f),
                Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
            auto* component = light->findComponent<LightComponent>();
            component->setShadowBias(0.1f);
            component->setShadowNormalBias(0.05f);
            component->setShadowDistance(20.0f);
            component->setShadowIntensity(0.5f);
            component->setShadowResolution(2048);
            component->setVsmBlurSize(16);
            component->setPenumbraSize(0.04f);
            component->setPenumbraFalloff(5.0f);
            _lights.push_back(light);
        }
        applyLightCount();
        applyShadowType();

        spdlog::info("Keys: 0-6 lights, T shadow type, [ / ] shadow alpha clip");
        return true;
    }

    void update(const float dt) override
    {
        handleKeys();

        // All lights turn together, each keeping its own azimuth offset
        _lightAngle += dt * 20.0f;
        for (size_t i = 0; i < _lights.size(); ++i) {
            _lights[i]->setLocalEulerAngles(55.0f, kLightAzimuths[i] + _lightAngle, 0.0f);
        }
    }

    void destroy() override
    {
        scene()->setSkybox(nullptr);
        scene()->setEnvAtlas(nullptr);
    }

private:
    void handleKeys()
    {
        auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        constexpr Key digits[] = {Key::Digit0, Key::Digit1, Key::Digit2, Key::Digit3, Key::Digit4, Key::Digit5,
            Key::Digit6};
        for (int count = 0; count < 7; ++count) {
            if (keyboard->wasPressed(digits[count])) {
                _lightCount = count;
                applyLightCount();
            }
        }
        if (keyboard->wasPressed(Key::T)) {
            _shadowType = (_shadowType + 1) % static_cast<int>(kShadowTypes.size());
            applyShadowType();
        }
        const float clip = _alphaClip;
        if (keyboard->wasPressed(Key::LeftBracket)) _alphaClip = std::max(0.0f, _alphaClip - 0.05f);
        if (keyboard->wasPressed(Key::RightBracket)) _alphaClip = std::min(1.0f, _alphaClip + 0.05f);
        if (_alphaClip != clip) {
            for (auto* model : _models) {
                model->setAlphaClip(_alphaClip);
            }
            spdlog::info("Shadow alpha clip {:.2f}", _alphaClip);
        }
    }

    void applyLightCount()
    {
        for (size_t i = 0; i < _lights.size(); ++i) {
            _lights[i]->setEnabled(static_cast<int>(i) < _lightCount);
        }
        spdlog::info("Lights: {}", _lightCount);
    }

    void applyShadowType()
    {
        for (auto* light : _lights) {
            light->findComponent<LightComponent>()->setShadowType(kShadowTypes[static_cast<size_t>(_shadowType)]);
        }
        spdlog::info("Shadow type: {}", kShadowTypeNames[static_cast<size_t>(_shadowType)]);
    }

    std::unique_ptr<Asset> _hdrAsset;
    std::unique_ptr<Texture> _skybox;
    std::unique_ptr<Texture> _envAtlas;
    std::vector<GSplatComponent*> _models;
    std::vector<Entity*> _lights;
    int _lightCount = 2;
    int _shadowType = 2;   // PCF5
    float _alphaClip = 0.4f;
    float _lightAngle = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(GsplatShadowsExample)
