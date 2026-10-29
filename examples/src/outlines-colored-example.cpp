// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/outlines-colored.
//
// The draco-compressed laboratory (scaled 100) under the helipad env atlas (skybox mip 2,
// exposure 2.5), every material made opaque with depth writes and its baked AO map removed,
// so the occlusion comes from SSAO alone. A directional light at (70, 135, 0) casts the
// shadows (4096 map, bias 0.4, normal offset bias 0.06, distance 600). The camera (clear
// (0.4, 0.45, 0.5), near 1, far 600, NEUTRAL tone mapping) starts at (-200, 100, 200) and
// orbits the laboratory's centre at the origin (pitch +/-90 degrees, zoom up to 300),
// through a camera frame with 4x MSAA and lighting-mode SSAO (radius 30, 12 samples,
// intensity 0.4, power 6, min angle 10, blurred, not randomized). Volumetric fog lit by the directional light (tint
// (1, 0.92, 0.8), density 0.01 below height -30, falloff 0.05, ambient 0.005, 600 units,
// 32 steps) is available but off. Three objects carry coloured outlines: the globe
// "Weltkugel" red, the chair "Stuhl" white and the telescope "Teleskop" green.
//
// Keys stand in for upstream's control panel:
//   1  SSAO on/off (default on)
//   2  Volumetric Fog on/off (default off)
// With both off the camera renders the scene directly, without the camera frame.
//
// @credit Laboratory by Sketchfab, CC BY 4.0
//
// DEVIATION: upstream composites the outlines before the UI layer, so the camera frame's
// tone mapping and fog do not touch them. The engine's OutlineRenderer blends its outline
// texture over the back buffer after the whole frame, which keeps them out of the tone
// mapping and the fog as well, but draws them over the UI layer too.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/extras/outlineRenderer.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/input/inputConstants.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/graphics/renderPassConstants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

class OutlinesColoredExample final: public ExampleApp
{
public:
    OutlinesColoredExample(): ExampleApp({.title = "Outlines Colored"}) {}

protected:
    bool create() override
    {
        _laboratoryAsset = std::make_unique<Asset>("laboratory", AssetType::CONTAINER,
            assetPath("models/laboratory.glb"));
        _helipadAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        auto* laboratory = _laboratoryAsset->resourceAs<ContainerResource>();
        auto* helipad = _helipadAsset->resourceAs<Texture>();
        if (!laboratory || !helipad) {
            spdlog::error("Failed to load the laboratory or the helipad env atlas");
            return false;
        }

        // Setup skydome
        scene()->setEnvAtlas(helipad);
        scene()->setSkyboxMip(2);
        scene()->setExposure(2.5f);

        // Get the instance of the laboratory
        _laboratory = laboratory->instantiateRenderEntity();
        _laboratory->setEngine(engine());
        _laboratory->setLocalScale(100.0f, 100.0f, 100.0f);
        root()->addChild(_laboratory);

        // Set up materials
        for (auto* render : _laboratory->findComponents<RenderComponent>()) {
            render->setCastShadows(true);
            render->setReceiveShadows(true);
            for (auto* meshInstance : render->meshInstances()) {
                auto* material = meshInstance ? dynamic_cast<StandardMaterial*>(meshInstance->material()) : nullptr;
                if (!material) {
                    continue;
                }

                // Disable blending / enable depth writes
                material->setTransparent(false);
                material->setBlendState(BlendState::noBlend());
                material->setDepthState(std::make_shared<DepthState>());

                // Disable baked AO map as we want to use SSAO only
                material->setAoMap(nullptr);
            }
        }

        // Create a directional light casting shadows, which also lights the volumetric fog. It
        // is low behind the laboratory, shining towards the camera, so the walls cast shafts of
        // light through the fog.
        auto* light = createDirectionalLight(Vector3(70.0f, 135.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        light->setName("DirectionalLight");
        if (auto* lightComponent = light->findComponent<LightComponent>()) {
            lightComponent->setShadowResolution(4096);
            lightComponent->setShadowBias(0.4f);
            lightComponent->setShadowNormalBias(0.06f);
            lightComponent->setShadowDistance(600.0f);
        }

        // Create an Entity with a camera component
        auto* camera = createCamera(Vector3(-60.0f, 30.0f, 60.0f));
        camera->setName("SceneCamera");
        _cameraComponent = camera->findComponent<CameraComponent>();
        if (!_cameraComponent || !_cameraComponent->camera()) {
            spdlog::error("Failed to create the camera");
            return false;
        }
        _cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        _cameraComponent->camera()->setNearClip(1.0f);
        _cameraComponent->camera()->setFarClip(600.0f);
        _cameraComponent->setToneMapping(TONEMAP_NEUTRAL);

        // Orbit the laboratory's bounds centre from 1.5 x its largest half extent over
        // sin(fov / 2), at most 300: (-200, 100, 200) around the origin
        const BoundingBox bounds = entityBounds(_laboratory);
        const Vector3 focusPoint = bounds.center();
        const Vector3& halfExtents = bounds.halfExtents();
        const float radius = std::max({halfExtents.getX(), halfExtents.getY(), halfExtents.getZ()});
        const float halfFov = 0.5f * _cameraComponent->camera()->fov() * std::numbers::pi_v<float> / 180.0f;
        const float distance = std::min(radius * 1.5f / std::sin(halfFov), kDistanceMax);
        const Vector3 direction = (camera->position() - focusPoint).normalized();
        camera->setPosition(focusPoint + direction * distance);
        if (auto* controls = addOrbitControls(camera, focusPoint)) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(0.01f, kDistanceMax));
            controls->storeResetState();
        }

        // Set up the camera frame rendering, using the settings of the ambient occlusion
        // example. The scene target is RGBA16F.
        auto ssao = _cameraComponent->ssao();
        ssao.type = SSAOTYPE_LIGHTING;
        ssao.blurEnabled = true;
        ssao.radius = 30.0f;
        ssao.samples = 12;
        ssao.intensity = 0.4f;
        ssao.power = 6.0f;
        ssao.minAngle = 10.0f;
        ssao.scale = 1.0f;
        ssao.randomize = false;
        _cameraComponent->setSsao(ssao);

        // Volumetric fog lit by the directional light, denser near the floor of the
        // laboratory. A low ambient term keeps the fog in the shadows dark, for a strong
        // contrast with the shafts of light.
        auto& fog = _cameraComponent->volumetricFog();
        fog.tint[0] = 1.0f;
        fog.tint[1] = 0.92f;
        fog.tint[2] = 0.8f;
        fog.density = 0.01f;
        fog.heightBase = -30.0f;
        fog.heightFalloff = 0.05f;
        fog.ambientIntensity = 0.005f;
        fog.maxDistance = 600.0f;

        // without TAA to resolve the noise of the raymarch, more steps keep it low
        fog.steps = 32;

        applySettings();

        // Create the outline renderer, and add entities to it
        _outlineRenderer = std::make_unique<OutlineRenderer>(engine());
        addOutline("Weltkugel", Color(1.0f, 0.0f, 0.0f, 1.0f));
        addOutline("Stuhl", Color(1.0f, 1.0f, 1.0f, 1.0f));
        addOutline("Teleskop", Color(0.0f, 1.0f, 0.0f, 1.0f));

        spdlog::info("Keys: 1 = SSAO, 2 = volumetric fog, Esc = quit");
        return true;
    }

    void update(float /*dt*/) override
    {
        handleKeys();

        // Update the outline renderer each frame
        if (_outlineRenderer && _cameraComponent) {
            _outlineRenderer->frameUpdate(_cameraComponent->entity());
        }
    }

    void destroy() override
    {
        // It owns a camera entity and renderer passes, released while the engine is alive
        _outlineRenderer.reset();
        if (scene()) {
            scene()->setEnvAtlas(nullptr);
        }
    }

private:
    static constexpr float kDistanceMax = 300.0f;

    void addOutline(const char* name, const Color& color)
    {
        if (auto* entity = dynamic_cast<Entity*>(_laboratory->findByName(name))) {
            _outlineRenderer->addEntity(entity, color);
        } else {
            spdlog::warn("The laboratory has no node named {}", name);
        }
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        bool changed = false;
        if (keyboard->wasPressed(Key::Digit1)) {
            _ssao = !_ssao;
            changed = true;
        }
        if (keyboard->wasPressed(Key::Digit2)) {
            _volumetricFog = !_volumetricFog;
            changed = true;
        }
        if (changed) {
            applySettings();
            spdlog::info("SSAO: {}, volumetric fog: {}", _ssao ? "on" : "off", _volumetricFog ? "on" : "off");
        }
    }

    void applySettings()
    {
        auto ssao = _cameraComponent->ssao();
        ssao.enabled = _ssao;
        _cameraComponent->setSsao(ssao);
        _cameraComponent->volumetricFog().enabled = _volumetricFog;

        // With neither effect enabled, the camera renders the scene directly: the frame's 4x
        // MSAA scene target would otherwise keep the camera frame alive on its own
        auto rendering = _cameraComponent->rendering();
        rendering.samples = (_ssao || _volumetricFog) ? 4 : 1;
        _cameraComponent->setRendering(rendering);
    }

    std::unique_ptr<Asset> _laboratoryAsset;
    std::unique_ptr<Asset> _helipadAsset;
    std::unique_ptr<OutlineRenderer> _outlineRenderer;
    Entity* _laboratory = nullptr;
    CameraComponent* _cameraComponent = nullptr;

    // Initial settings
    bool _ssao = true;
    bool _volumetricFog = false;
};

VISUTWIN_EXAMPLE_MAIN(OutlinesColoredExample)
