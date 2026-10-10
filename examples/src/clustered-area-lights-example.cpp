// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream graphics/clustered-area-lights.
//
// A 9 x 9 grid of area lights, 5 units apart and 0.6 above a 45 x 45 seaside-rocks
// ground plane (colour, normal and gloss maps tiled 17x; grey, gloss 0.8, metalness
// 0.7). Each light picks a random colour (every channel up to 0.7) and a random shape:
// 30% a sphere-shaped omni (scale 1.5, range 6), 30% a disk-shaped spot (scale 1.5,
// range 5) and 40% a 2 x 1 rect-shaped spot (range 5), all at intensity 4 with an
// inverse-squared falloff and cone angles 88 / 89. The spots are turned to shine
// sideways along +Z. Every light carries an emissive primitive matching its shape
// (sphere, flattened cylinder, flattened box) at ten times its intensity, and a spot a
// black copy over its back. Clustered lighting runs a 30 x 2 x 30 grid with up to 20
// lights per cell, area lights on and shadows off. The camera frame multisamples 4x,
// blooms at 0.01 over 4 levels and tone-maps NEUTRAL; an orbit camera starts at
// (3, 3, 12) looking at the ground's centre, out to a distance of 60.
//
// Keys stand in for upstream's control panel (the ground material):
//   [ / ]  gloss -/+ 0.05, 0..1
//   ; / '  metalness -/+ 0.05, 0..1
//
// DEVIATIONS:
// - upstream loads the LTC LUTs from a JSON asset; this engine has them built in.
// - the colours and shapes come from a SEEDED generator in place of upstream's
//   Math.random(), so the grid is the same in every run (and comparable across runs).
// - upstream's orbitCamera script (inertia 0.2, distanceMax 60) is the examples'
//   CameraControls in orbit mode around the same pivot; there is no inertia.
// - upstream asks for an HDR display format and skips tone mapping on an HDR display;
//   the port always renders to a standard display with NEUTRAL tone mapping.
// - upstream caps the pixel ratio at 2; the examples render at one pixel per point.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "platform/input/inputConstants.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kGroundTiling = 17.0f;
    constexpr float kMaterialStep = 0.05f;
}

class ClusteredAreaLightsExample final: public ExampleApp
{
public:
    ClusteredAreaLightsExample(): ExampleApp({.title = "Clustered Area Lights"}) {}

protected:
    bool create() override
    {
        // Clustered lighting sized for many lights: a fine grid in x and z, a modest
        // per-cell budget, area lights on and no shadows.
        scene()->setClusteredLightingEnabled(true);
        auto& lighting = scene()->lighting();
        lighting.cellsX = 30;
        lighting.cellsY = 2;
        lighting.cellsZ = 30;
        lighting.maxLightsPerCell = 20;
        lighting.areaLightsEnabled = true;
        lighting.shadowsEnabled = false;

        _colorAsset = std::make_unique<Asset>("color", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-color.jpg"), AssetData{.mipmaps = true});
        _normalAsset = std::make_unique<Asset>("normal", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-normal.jpg"), AssetData{.mipmaps = true});
        _glossAsset = std::make_unique<Asset>("gloss", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-gloss.jpg"), AssetData{.mipmaps = true});

        Texture* colorTexture = requireTexture(_colorAsset, "seaside-rocks01-color");
        Texture* normalTexture = requireTexture(_normalAsset, "seaside-rocks01-normal");
        Texture* glossTexture = requireTexture(_glossAsset, "seaside-rocks01-gloss");
        if (!colorTexture || !normalTexture || !glossTexture) {
            return false;
        }

        // Pure black material, used on the back side of the spot light shapes.
        _blackMaterial = std::make_shared<StandardMaterial>();
        _blackMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _blackMaterial->setUseLighting(false);

        // Ground material.
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.5f, 1.0f));
        _groundMaterial->setGloss(_gloss);
        _groundMaterial->setMetalness(_metalness);
        _groundMaterial->setUseMetalness(true);
        _groundMaterial->setDiffuseMap(colorTexture);
        _groundMaterial->setNormalMap(normalTexture);
        _groundMaterial->setGlossMap(glossTexture);
        const Vector2 groundTiling(kGroundTiling, kGroundTiling);
        _groundMaterial->setDiffuseMapTiling(groundTiling);
        _groundMaterial->setNormalMapTiling(groundTiling);
        _groundMaterial->setGlossMapTiling(groundTiling);

        auto* ground = new Entity();
        ground->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(ground->addComponent<RenderComponent>())) {
            render->setMaterial(_groundMaterial.get());
            render->setType("plane");
        }
        ground->setLocalPosition(0.0f, 0.0f, 0.0f);
        ground->setLocalScale(45.0f, 1.0f, 45.0f);
        root()->addChild(ground);

        // Camera, orbiting the ground's centre.
        auto* camera = createCamera(Vector3(3.0f, 3.0f, 12.0f));
        if (auto* cameraComp = camera->findComponent<CameraComponent>()) {
            if (cameraComp->camera()) {
                cameraComp->camera()->setClearColor(Color(0.05f, 0.05f, 0.05f, 1.0f));
                cameraComp->camera()->setFov(60.0f);
                cameraComp->camera()->setFarClip(1000.0f);
            }

            // Camera frame: 4x MSAA, bloom 0.01 over 4 levels, NEUTRAL tone mapping.
            cameraComp->setToneMapping(TONEMAP_NEUTRAL);
            auto rendering = cameraComp->rendering();
            rendering.samples = 4;
            rendering.bloomIntensity = 0.01f;
            rendering.bloomBlurLevel = 4;
            cameraComp->setRendering(rendering);
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setZoomRange(Vector2(0.0f, 60.0f));
            controls->storeResetState();
        }

        // A grid of area lights of sphere, disk and rect shapes.
        std::uniform_real_distribution<float> random(0.0f, 1.0f);
        for (int x = -20; x <= 20; x += 5) {
            for (int y = -20; y <= 20; y += 5) {
                const Vector3 position(static_cast<float>(x), 0.6f, static_cast<float>(y));
                const float r = random(_rng) * 0.7f;
                const float g = random(_rng) * 0.7f;
                const float b = random(_rng) * 0.7f;
                const Color color(r, g, b, 1.0f);
                const float rand = random(_rng);
                if (rand < 0.3f) {
                    createAreaLight(LightType::LIGHTTYPE_OMNI, LightShape::LIGHTSHAPE_SPHERE, position,
                        Vector3(1.5f, 1.5f, 1.5f), color, 4.0f, 6.0f);
                } else if (rand < 0.6f) {
                    createAreaLight(LightType::LIGHTTYPE_SPOT, LightShape::LIGHTSHAPE_DISK, position,
                        Vector3(1.5f, 1.5f, 1.5f), color, 4.0f, 5.0f);
                } else {
                    createAreaLight(LightType::LIGHTTYPE_SPOT, LightShape::LIGHTSHAPE_RECT, position,
                        Vector3(2.0f, 1.0f, 1.0f), color, 4.0f, 5.0f);
                }
            }
        }

        spdlog::info("81 clustered area lights (sphere, disk and rect) over a seaside-rocks ground");
        spdlog::info("Keys: [ ] gloss, ; ' metalness, R reset camera, Esc quit");
        return true;
    }

    void update(float /*dt*/) override
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }

        const float gloss = adjust(_gloss, keyboard->wasPressed(Key::LeftBracket),
            keyboard->wasPressed(Key::RightBracket));
        const float metalness = adjust(_metalness, keyboard->wasPressed(Key::Semicolon),
            keyboard->wasPressed(Key::Apostrophe));
        if (gloss == _gloss && metalness == _metalness) {
            return;
        }

        _gloss = gloss;
        _metalness = metalness;
        _groundMaterial->setGloss(_gloss);
        _groundMaterial->setMetalness(_metalness);
        spdlog::info("Ground material: gloss {:.2f}, metalness {:.2f}", _gloss, _metalness);
    }

private:
    static Texture* requireTexture(const std::unique_ptr<Asset>& asset, const char* label)
    {
        Texture* texture = asset->resourceAs<Texture>();
        if (!texture) {
            spdlog::error("Failed to load texture asset '{}'", label);
        }
        return texture;
    }

    // A slider value stepped down and up, clamped to [0, 1] and kept at the panel's
    // two decimals.
    static float adjust(const float value, const bool down, const bool up)
    {
        float result = value;
        if (down) {
            result -= kMaterialStep;
        }
        if (up) {
            result += kMaterialStep;
        }
        return std::round(std::clamp(result, 0.0f, 1.0f) * 100.0f) / 100.0f;
    }

    // An area light, with its visual: an emissive primitive matching the light's shape
    // and, for a spot, a black primitive over the back that emits nothing.
    void createAreaLight(const LightType type, const LightShape shape, const Vector3& position,
        const Vector3& scale, const Color& color, const float intensity, const float range)
    {
        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* lightComp = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lightComp->setType(type);
            lightComp->setShape(shape);
            lightComp->setColor(color);
            lightComp->setIntensity(intensity);
            lightComp->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
            lightComp->setRange(range);
            lightComp->setInnerConeAngle(88.0f);
            lightComp->setOuterConeAngle(89.0f);
        }

        light->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
        light->setLocalPosition(position.getX(), position.getY(), position.getZ());
        if (type == LightType::LIGHTTYPE_SPOT) {
            light->rotate(-90.0f, 0.0f, 0.0f);
        }
        root()->addChild(light);

        // Emissive material in the light source's colour.
        auto brightMaterial = std::make_shared<StandardMaterial>();
        brightMaterial->setEmissive(color);
        brightMaterial->setEmissiveIntensity(intensity * 10.0f);
        brightMaterial->setUseLighting(false);
        _materials.push_back(brightMaterial);

        // Primitive matching the light's shape, flattened to a disk or a rectangle.
        const char* primitive = shape == LightShape::LIGHTSHAPE_SPHERE ? "sphere"
            : shape == LightShape::LIGHTSHAPE_DISK ? "cylinder" : "box";
        const float primitiveHeight = shape != LightShape::LIGHTSHAPE_SPHERE ? 0.001f : 1.0f;

        auto* brightShape = new Entity();
        brightShape->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(brightShape->addComponent<RenderComponent>())) {
            render->setMaterial(brightMaterial.get());
            render->setType(primitive);
        }
        brightShape->setLocalScale(1.0f, primitiveHeight, 1.0f);
        light->addChild(brightShape);

        // The back of a spot's source, which emits no light.
        if (type == LightType::LIGHTTYPE_SPOT) {
            auto* blackShape = new Entity();
            blackShape->setEngine(engine());
            if (auto* render = static_cast<RenderComponent*>(blackShape->addComponent<RenderComponent>())) {
                render->setMaterial(_blackMaterial.get());
                render->setType(primitive);
            }
            blackShape->setLocalPosition(0.0f, 0.004f, 0.0f);
            blackShape->setLocalEulerAngles(-180.0f, 0.0f, 0.0f);
            blackShape->setLocalScale(1.0f, primitiveHeight, 1.0f);
            light->addChild(blackShape);
        }
    }

    std::unique_ptr<Asset> _colorAsset;
    std::unique_ptr<Asset> _normalAsset;
    std::unique_ptr<Asset> _glossAsset;

    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::shared_ptr<StandardMaterial> _blackMaterial;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;

    std::mt19937 _rng{20261010};
    float _gloss = 0.8f;
    float _metalness = 0.7f;
};

VISUTWIN_EXAMPLE_MAIN(ClusteredAreaLightsExample)
