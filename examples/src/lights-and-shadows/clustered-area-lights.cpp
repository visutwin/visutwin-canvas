// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream lights-and-shadows/clustered-area-lights.
//
// 81 clustered area lights float just above a metallic floor (the seaside-rocks set tiled 28 x 28
// on an 80 x 80 plane, gloss 0.8, metalness 0.7) in a sunflower spiral: each turned by the golden
// angle from the previous one and placed further out, which spreads them evenly over a disk of
// radius 14, their colours going around the colour wheel from the centre out. They cycle sphere,
// disk and rectangle shapes, all omni lights of range 3 (inverse-squared), so the rectangles and
// disks shine from both sides; each carries an emissive source of its shape. They float in a wave
// rolling out from the centre, the disks and rectangles standing upright and spinning slowly. The
// clustered grid is 30 x 2 x 30 with up to 20 lights a cell, no shadows. The floor fades into the
// black night through linear fog (22 - 50). The camera frame (4x MSAA, neutral tone mapping) adds
// bloom 0.03 over 7 levels and a vignette.
//
// Keys stand in for upstream's controls panel:
//   A            area lights on or off (off: the lights are punctual)
//   [ / ]        the lights' intensity -/+ 0.1 (0 - 3)
//   B / N        bloom -/+ 0.01 (0 - 0.1)
//   - / =        the floor's gloss -/+ 0.05      9 / 0   its metalness -/+ 0.05
//   Space        pause the animation
//
// DEVIATIONS:
// - The LTC lookup tables are built into the engine (upstream loads them from a JSON asset).
// - Tone mapping is NEUTRAL always (upstream drops it on an HDR display, which is not detected here).
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include "exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kLightCount = 81;
    constexpr float kSpiralRadius = 14.0f;
    constexpr float kEmissiveScale = 1.5f;   // the sources' brightness relative to their light

    // A colour of a hue around the colour wheel, 0-1.
    Color hueColor(const float hue)
    {
        const auto channel = [hue](const float offset) {
            return 0.5f + 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * (hue + offset));
        };
        return Color(channel(0.0f), channel(2.0f / 3.0f), channel(1.0f / 3.0f), 1.0f);
    }
}

class ClusteredAreaLightsExample final : public ExampleApp
{
public:
    ClusteredAreaLightsExample() : ExampleApp({}) {}

protected:
    bool create() override
    {
        _colorAsset = std::make_unique<Asset>("color", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-color.jpg"), AssetData{.mipmaps = true});
        _normalAsset = std::make_unique<Asset>("normal", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-normal.jpg"), AssetData{.mipmaps = true});
        _glossAsset = std::make_unique<Asset>("gloss", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-gloss.jpg"), AssetData{.mipmaps = true});
        Texture* color = _colorAsset->resourceAs<Texture>();
        Texture* normal = _normalAsset->resourceAs<Texture>();
        Texture* gloss = _glossAsset->resourceAs<Texture>();
        if (!color || !normal || !gloss) {
            spdlog::error("clustered-area-lights: a texture did not load");
            return false;
        }

        // The clustered lighting subdivides the space the lights occupy into cells, each holding
        // the lights overlapping it; the small ranges keep that well below the per-cell limit.
        scene()->setClusteredLightingEnabled(true);
        auto& lighting = scene()->lighting();
        lighting.cellsX = 30;
        lighting.cellsY = 2;
        lighting.cellsZ = 30;
        lighting.maxLightsPerCell = 20;
        lighting.areaLightsEnabled = true;
        lighting.shadowsEnabled = false;

        // The metallic floor the lights are reflected in.
        _floorMaterial = std::make_shared<StandardMaterial>();
        _floorMaterial->setDiffuse(Color(0.5f, 0.5f, 0.5f, 1.0f));
        _floorMaterial->setDiffuseMap(color);
        _floorMaterial->setNormalMap(normal);
        _floorMaterial->setGlossMap(gloss);
        _floorMaterial->setUseMetalness(true);
        const Vector2 tiling(28.0f, 28.0f);
        _floorMaterial->setDiffuseMapTiling(tiling);
        _floorMaterial->setNormalMapTiling(tiling);
        _floorMaterial->setGlossMapTiling(tiling);
        createPrimitive("plane", _floorMaterial.get(), Vector3(0.0f, 0.0f, 0.0f), Vector3(80.0f, 1.0f, 80.0f));

        // The lights, in a sunflower spiral.
        const struct
        {
            LightShape shape;
            const char* primitive;
            Vector3 scale;
        } shapes[] = {
            {LightShape::LIGHTSHAPE_SPHERE, "sphere", Vector3(0.9f, 0.9f, 0.9f)},
            {LightShape::LIGHTSHAPE_DISK, "cylinder", Vector3(1.3f, 1.3f, 1.3f)},
            {LightShape::LIGHTSHAPE_RECT, "box", Vector3(1.6f, 1.0f, 0.9f)},
        };
        const float goldenAngle = std::numbers::pi_v<float> * (3.0f - std::sqrt(5.0f));
        for (int i = 0; i < kLightCount; ++i) {
            const auto& shape = shapes[i % 3];
            const float radius = kSpiralRadius * std::sqrt((static_cast<float>(i) + 0.5f) / kLightCount);
            const float angle = static_cast<float>(i) * goldenAngle;
            const Color lightColor = hueColor(radius / kSpiralRadius);

            FloatingLight light;
            light.entity = new Entity();
            light.entity->setEngine(engine());
            light.light = static_cast<LightComponent*>(light.entity->addComponent<LightComponent>());
            if (light.light) {
                light.light->setType(LightType::LIGHTTYPE_OMNI);
                light.light->setShape(shape.shape);
                light.light->setColor(lightColor);
                light.light->setRange(3.0f);
                light.light->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
            }
            light.entity->setLocalScale(shape.scale.getX(), shape.scale.getY(), shape.scale.getZ());
            root()->addChild(light.entity);

            // The visible source; the rectangle and the disk in the light's XZ plane.
            light.material = std::make_shared<StandardMaterial>();
            light.material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
            light.material->setEmissive(lightColor);
            light.material->setUseLighting(false);
            const bool sphere = shape.shape == LightShape::LIGHTSHAPE_SPHERE;
            Entity* source = createPrimitive(shape.primitive, light.material.get());
            light.entity->addChild(source);
            source->setLocalPosition(0.0f, 0.0f, 0.0f);
            source->setLocalScale(1.0f, sphere ? 1.0f : 0.002f, 1.0f);

            light.position = Vector3(std::sin(angle) * radius, 0.0f, std::cos(angle) * radius);
            light.height = sphere ? 0.7f : 0.8f;
            light.flat = !sphere;
            light.phase = angle;
            _lights.push_back(light);
        }

        // The floor fades into the night in the distance; the clear colour and the fog are black.
        scene()->setFogEnabled(true);
        scene()->setFogType(FogType::FOG_LINEAR);
        scene()->setFogColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        scene()->setFogLinear(22.0f, 50.0f);

        // The camera, low above the floor, so the lights are reflected in it.
        Entity* camera = createCamera(Vector3(0.0f, 4.5f, 16.0f));
        _cameraComponent = camera->findComponent<CameraComponent>();
        _cameraComponent->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _cameraComponent->camera()->setFov(60.0f);
        _cameraComponent->camera()->setFarClip(200.0f);
        _cameraComponent->setToneMapping(TONEMAP_NEUTRAL);
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.3f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-80.0f, -2.0f));
            controls->setZoomRange(Vector2(3.0f, 30.0f));
            controls->storeResetState();
        }
        auto rendering = _cameraComponent->rendering();
        rendering.samples = 4;
        rendering.bloomBlurLevel = 7;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.4f;
        _cameraComponent->setRendering(rendering);

        applySettings();
        update(0.0f);
        spdlog::info("clustered-area-lights: A area lights, [ ] intensity, B/N bloom, - = gloss, 9/0 metalness, "
                     "Space pause");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_A: _areaLights = !_areaLights; break;
        case SDLK_LEFTBRACKET: _intensity = std::max(0.0f, _intensity - 0.1f); break;
        case SDLK_RIGHTBRACKET: _intensity = std::min(3.0f, _intensity + 0.1f); break;
        case SDLK_B: _bloom = std::max(0.0f, _bloom - 0.01f); break;
        case SDLK_N: _bloom = std::min(0.1f, _bloom + 0.01f); break;
        case SDLK_MINUS: _gloss = std::max(0.0f, _gloss - 0.05f); break;
        case SDLK_EQUALS: _gloss = std::min(1.0f, _gloss + 0.05f); break;
        case SDLK_9: _metalness = std::max(0.0f, _metalness - 0.05f); break;
        case SDLK_0: _metalness = std::min(1.0f, _metalness + 0.05f); break;
        case SDLK_SPACE: _animate = !_animate; break;
        default: return false;
        }
        applySettings();
        return true;
    }

    void update(const float dt) override
    {
        if (_animate) {
            _time += dt;
        }

        // A wave rolling out from the centre; the disks and rectangles stand upright (their XZ
        // plane turned vertical) and spin slowly about the vertical axis.
        for (const auto& light : _lights) {
            const float distance = light.position.length();
            light.entity->setLocalPosition(light.position.getX(),
                light.height + std::sin(_time * 1.5f - distance * 0.5f) * 0.2f, light.position.getZ());
            if (light.flat) {
                const float spin = std::fmod(light.phase * 180.0f / std::numbers::pi_v<float> + _time * 15.0f, 360.0f);
                light.entity->setLocalEulerAngles(90.0f, spin, 0.0f);
            }
        }
    }

private:
    struct FloatingLight
    {
        Entity* entity = nullptr;
        LightComponent* light = nullptr;
        std::shared_ptr<StandardMaterial> material;
        Vector3 position;
        float height = 0.0f;
        bool flat = false;
        float phase = 0.0f;
    };

    void applySettings()
    {
        constexpr float kIntensity = 4.0f;
        scene()->lighting().areaLightsEnabled = _areaLights;
        for (auto& light : _lights) {
            if (light.light) {
                light.light->setIntensity(kIntensity * _intensity);
            }
            light.material->setEmissiveIntensity(kIntensity * _intensity * kEmissiveScale);
        }
        auto rendering = _cameraComponent->rendering();
        rendering.bloomIntensity = _bloom;
        _cameraComponent->setRendering(rendering);
        _floorMaterial->setGloss(_gloss);
        _floorMaterial->setMetalness(_metalness);
    }

    std::unique_ptr<Asset> _colorAsset;
    std::unique_ptr<Asset> _normalAsset;
    std::unique_ptr<Asset> _glossAsset;
    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::vector<FloatingLight> _lights;
    CameraComponent* _cameraComponent = nullptr;

    bool _areaLights = true;
    float _intensity = 1.0f;
    float _bloom = 0.03f;
    float _gloss = 0.8f;
    float _metalness = 0.7f;
    bool _animate = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ClusteredAreaLightsExample)
