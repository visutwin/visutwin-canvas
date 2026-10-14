// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.10.2026
//
// Port of upstream test/material-instance-overrides.
//
// Mesh instance overrides of material uniforms. Five spheres (scale 1.5, 1.9 apart) share ONE
// StandardMaterial (gloss 0.4) whose diffuse colour cycles slowly; each mesh instance may override
// that material's uniforms (MeshInstance::setParameter), which gives its draw a copy of the
// material's block with the override applied. The copies must follow the material's own changes
// while keeping their overrides:
//   1. "material"                     no override: the shared colour
//   2. "override: diffuse"            a static red diffuse
//   3. "override: animated"           a diffuse whose hue cycles quickly, set every frame
//   4. "override: toggled"            a red diffuse set and removed every 1.5 seconds
//   5. "override: diffuse + emissive + gloss"   three overrides through the one copy
// The helipad environment atlas lights them, with a warm directional light (intensity 2) at
// (45, 30, 0); the camera at (0, 1.4, 11.5) looks at the origin with ACES tone mapping.
//
// DEVIATION: an override takes the engine's own parameter convention, so colours are authored
// sRGB as the material's setters take them (decoded on the GPU, or on the CPU for emissive):
// upstream writes LINEAR values straight into the uniform, so its hue colours are raised to 2.2
// before they are set and its emissive (0.35, 0.12, 0.02) is linear. The same colours are given
// here gamma-encoded. Text size is an integer here, so the labels are laid out at 32 and their
// entities scaled to upstream's 0.2.

#include <cmath>
#include <memory>
#include <string>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kPi = 3.14159265358979f;

    // A hue (0..1) at saturation 0.7, value 0.9, as a gamma-space colour.
    Color hueToColor(float hue)
    {
        constexpr float saturation = 0.7f;
        constexpr float value = 0.9f;
        hue -= std::floor(hue);
        const int sector = static_cast<int>(std::floor(hue * 6.0f)) % 6;
        const float f = hue * 6.0f - std::floor(hue * 6.0f);
        const float p = value * (1.0f - saturation);
        const float q = value * (1.0f - f * saturation);
        const float t = value * (1.0f - (1.0f - f) * saturation);
        const float table[6][3] = {
            {value, t, p}, {q, value, p}, {p, value, t}, {p, q, value}, {t, p, value}, {value, p, q}};
        return Color(table[sector][0], table[sector][1], table[sector][2], 1.0f);
    }

    // Upstream's labels are 0.2 units per em; text size here is an integer.
    constexpr int kLabelFontSize = 32;
    constexpr float kLabelScale = 0.2f / static_cast<float>(kLabelFontSize);
}

class MaterialInstanceOverridesExample final : public ExampleApp
{
public:
    MaterialInstanceOverridesExample() : ExampleApp({.title = "Material Instance Overrides"}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options, {.screen = false});
    }

    bool create() override
    {
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _font = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        auto* helipad = _helipad->resourceAs<Texture>();
        _fontResource = _font->resourceAs<FontResource>();
        if (!helipad || !_fontResource) {
            spdlog::error("material-instance-overrides: an asset failed to load");
            return false;
        }
        scene()->setEnvAtlas(helipad);

        Entity* camera = createCamera(Vector3(0.0f, 1.4f, 11.5f));
        camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        cameraComponent->setToneMapping(TONEMAP_ACES);
        cameraComponent->camera()->setClearColor(Color(0.1f, 0.1f, 0.12f, 1.0f));

        createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f), Color(1.0f, 0.95f, 0.85f, 1.0f), 2.0f);

        // The shared material
        _shared = std::make_shared<StandardMaterial>();
        _shared->setDiffuse(Color(0.6f, 0.6f, 0.6f, 1.0f));
        _shared->setGloss(0.4f);

        // 1. no overrides
        createSphere("material", -3.8f);

        // 2. a static override of the diffuse colour only
        createSphere("override: diffuse", -1.9f)->setParameter("material_diffuse", hueToColor(0.0f));

        // 3. an override changing every frame
        _animated = createSphere("override: animated", 0.0f);

        // 4. an override set and removed every 1.5 seconds
        _toggled = createSphere("override: toggled", 1.9f);

        // 5. diffuse, emissive and gloss overrides, applied through the same copy of the block
        MeshInstance* mixed = createSphere("override: diffuse\n+ emissive + gloss", 3.8f);
        mixed->setParameter("material_diffuse", hueToColor(0.6f));
        mixed->setParameter("material_emissive", Color(std::pow(0.35f, 1.0f / 2.2f), std::pow(0.12f, 1.0f / 2.2f),
            std::pow(0.02f, 1.0f / 2.2f), 1.0f));
        mixed->setParameter("material_gloss", 0.9f);
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // The shared material cycles its own diffuse colour slowly; every copy follows it
        const float wave = 0.4f * std::sin(_time * 0.05f * kPi * 2.0f);
        _shared->setDiffuse(Color(0.6f + wave, 0.6f, 0.6f - wave, 1.0f));

        // The animated override cycles quickly
        _animated->setParameter("material_diffuse", hueToColor(_time * 0.5f));

        // The toggled override comes and goes
        _toggleTime += dt;
        if (_toggleTime > 1.5f) {
            _toggleTime = 0.0f;
            _toggleOn = !_toggleOn;
            if (_toggleOn) {
                _toggled->setParameter("material_diffuse", hueToColor(0.0f));
            } else {
                _toggled->deleteParameter("material_diffuse");
            }
        }
    }

private:
    // A labelled sphere with the shared material; returns its mesh instance.
    MeshInstance* createSphere(const std::string& label, const float x)
    {
        auto* root = new Entity();
        root->setEngine(engine());
        root->setName(label);
        root->setLocalPosition(x, 0.0f, 0.0f);
        this->root()->addChild(root);

        auto* sphere = new Entity();
        sphere->setEngine(engine());
        sphere->setName("sphere");
        auto* render = static_cast<RenderComponent*>(sphere->addComponent<RenderComponent>());
        render->setMaterial(_shared.get());
        render->setType("sphere");
        sphere->setLocalScale(1.5f, 1.5f, 1.5f);
        root->addChild(sphere);

        auto* text = new Entity();
        text->setEngine(engine());
        text->setName("label");
        auto* element = static_cast<ElementComponent*>(text->addComponent<ElementComponent>());
        element->setType(ElementType::Text);
        element->setFontResource(_fontResource);
        element->setFontSize(kLabelFontSize);
        element->setPivot(Vector2(0.5f, 0.5f));
        element->setAnchor(Vector4(0.5f, 0.5f, 0.5f, 0.5f));
        element->setText(label);
        text->setLocalPosition(0.0f, 1.15f, 0.0f);
        text->setLocalScale(kLabelScale, kLabelScale, kLabelScale);
        root->addChild(text);

        return render->meshInstances().front();
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _font;
    FontResource* _fontResource = nullptr;
    std::shared_ptr<StandardMaterial> _shared;
    MeshInstance* _animated = nullptr;
    MeshInstance* _toggled = nullptr;
    float _time = 0.0f;
    float _toggleTime = 0.0f;
    bool _toggleOn = false;
};

VISUTWIN_EXAMPLE_MAIN(MaterialInstanceOverridesExample)
