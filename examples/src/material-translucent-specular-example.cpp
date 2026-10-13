// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream materials/material-translucent-specular.
//
// A 10 x 5 grid of alpha-blended grey spheres (scale 0.7, one unit apart, centred on the
// origin): diffuse 0.7, white specular, metalness 0 in the metalness workflow, the gloss
// rising from 0.5 to 1 bottom to top. Across each half of the grid the opacity rises from
// 0.04 to 0.81 ((column / 5 + 0.2) squared). The left half fades its specular with the
// opacity; the right half keeps its highlights and reflections at any opacity (the
// specular's luminance is added to the opacity), as glass does. Every material blends
// normally without writing alpha, and keeps writing depth. Two world-space text labels,
// "Spec Fade On" and "Spec Fade Off", sit under the halves. Lit by the helipad environment
// atlas (drawn as the skybox at mip 1, intensity 1) and three white directional lights at
// (60 + 10 i, 30 + 90 i, 0) degrees, seen by a fixed camera at (0, 0, 8) with ACES.
//
// Upstream's example has no controls:
//   F1 HUD | Esc quit
//
// DEVIATION: ElementComponent::setFontSize takes an int, so upstream's font size of 0.5 is
// font size 64 on an entity scaled by 0.5 / 64: the same size in world units.
//
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "platform/graphics/blendState.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int NUM_SPHERES_X = 10;
    constexpr int NUM_SPHERES_Z = 5;

    // Font size 64 scaled down to 0.5 world units, see the header.
    constexpr int kTextFontSize = 64;
    constexpr float kTextWorldSize = 0.5f;
}

class MaterialTranslucentSpecularExample final : public ExampleApp
{
public:
    MaterialTranslucentSpecularExample() : ExampleApp({.title = "Material Translucent Specular"}) {}

protected:
    void configure(AppOptions& options) override
    {
        // The labels are world-space text elements, on no screen.
        registerUi(options, {.screen = false});
    }

    bool create() override
    {
        _helipadAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        Texture* helipad = _helipadAsset->resourceAs<Texture>();
        _font = _fontAsset->resourceAs<FontResource>();
        if (!helipad || !_font) {
            spdlog::error("Failed to load cubemaps/helipad-env-atlas.png or fonts/roboto-regular.json");
            return false;
        }

        scene()->setEnvAtlas(helipad);
        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(1.0f);

        // Create an entity with a camera component
        Entity* camera = createCamera(Vector3(0.0f, 0.0f, 8.0f));
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->setToneMapping(TONEMAP_ACES);
        }

        // Create entities with directional light components
        for (int i = 0; i < 3; ++i) {
            createDirectionalLight(Vector3(60.0f + 10.0f * static_cast<float>(i),
                30.0f + 90.0f * static_cast<float>(i), 0.0f));
        }

        // Normal alpha blending, leaving the target's alpha alone
        _blendState = std::make_shared<BlendState>(BlendState::alphaBlend());
        _blendState->setAlphaWrite(false);

        for (int i = 0; i < NUM_SPHERES_Z; ++i) {
            for (int j = 0; j < NUM_SPHERES_X; ++j) {
                createSphere(j, 0, i);
            }
        }

        createText("Spec Fade On", -NUM_SPHERES_X * 0.25f, (NUM_SPHERES_Z + 1) * -0.5f, 0.0f);
        createText("Spec Fade Off", NUM_SPHERES_X * 0.25f, (NUM_SPHERES_Z + 1) * -0.5f, 0.0f);

        return true;
    }

private:
    void createSphere(const int x, const int /*y*/, const int z)
    {
        const auto fx = static_cast<float>(x);
        const auto fz = static_cast<float>(z);

        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(0.7f, 0.7f, 0.7f, 1.0f));
        material->setSpecular(Color(1.0f, 1.0f, 1.0f, 1.0f));
        material->setMetalness(0.0f);
        material->setGloss(fz / (NUM_SPHERES_Z - 1) * 0.5f + 0.5f);
        material->setUseMetalness(true);
        material->setBlendState(_blendState);
        material->setTransparent(true);
        const float ramp = x >= 5 ? (fx - 5.0f) / 5.0f + 0.2f : fx / 5.0f + 0.2f;
        material->setOpacity(ramp * ramp);
        material->setOpacityFadesSpecular(!(x >= 5));

        createPrimitive("sphere", material.get(),
            Vector3(fx - (NUM_SPHERES_X - 1) * 0.5f, fz - (NUM_SPHERES_Z - 1) * 0.5f, 0.0f),
            Vector3(0.7f, 0.7f, 0.7f));
        _materials.push_back(std::move(material));
    }

    void createText(const std::string& message, const float x, const float y, const float z)
    {
        // Create a text element-based entity
        ElementComponent* element = createElement(engine(), root(), {
            .type = ElementType::Text,
            .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
            .pivot = Vector2(0.5f, 0.5f),
            .font = _font,
            .text = message,
            .fontSize = kTextFontSize
        });
        Entity* text = element->entity();
        constexpr float scale = kTextWorldSize / static_cast<float>(kTextFontSize);
        text->setLocalPosition(x, y, z);
        text->setLocalEulerAngles(0.0f, 0.0f, 0.0f);
        text->setLocalScale(scale, scale, scale);
    }

    std::unique_ptr<Asset> _helipadAsset;
    std::unique_ptr<Asset> _fontAsset;
    FontResource* _font = nullptr;
    std::shared_ptr<BlendState> _blendState;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
};

VISUTWIN_EXAMPLE_MAIN(MaterialTranslucentSpecularExample)
