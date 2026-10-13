// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream materials/material-transparency.
//
// Seven columns of ten red emissive spheres (scale 0.9, 1.5 units apart across, one unit up),
// comparing alpha blending with each opacity dither pattern: the first column blends
// normally, the others draw opaque and discard fragments against a Bayer 2x2, 4x4, 8x8 or
// 16x16 matrix, blue noise or interleaved gradient noise. Each sphere's material is white
// specular, metalness 0, gloss 0.5 in the metalness workflow, emissive red; up a column the
// perceived opacity rises in ten even steps, (row + 1) / 10, converted to linear with a 2.2
// power so the fade looks even. There are no lights and no environment, so only the emissive
// shows. A world-space two-line label names each column. A fixed camera at (0, -0.5, 16)
// clears to black with linear tone mapping, and the back buffer is NOT multisampled, which
// keeps the dither patterns crisp.
//
// Upstream's example has no controls:
//   F1 HUD | Esc quit
//
// DEVIATIONS:
// - upstream asks for an sRGB back buffer (honoured on WebGPU only; WebGL2 falls back to its
//   usual one). The port renders to the engine's usual back buffer with the gamma encode in
//   the shader, which is upstream's WebGL2 path: the alpha-blended column blends the
//   gamma-encoded colours.
// - ElementComponent::setFontSize takes an int, so upstream's font size of 0.3 is font size
//   64 on an entity scaled by 0.3 / 64: the same size in world units.
//
#include <cmath>
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
    constexpr int NUM_SPHERES_X = 7;
    constexpr int NUM_SPHERES_Z = 10;

    constexpr DitherMode kDitherOptions[NUM_SPHERES_X] = {
        DitherMode::DITHER_NONE,
        DitherMode::DITHER_BAYER2,
        DitherMode::DITHER_BAYER4,
        DitherMode::DITHER_BAYER8,
        DitherMode::DITHER_BAYER16,
        DitherMode::DITHER_BLUENOISE,
        DitherMode::DITHER_IGNNOISE
    };

    // The names of the dither modes, as the material and entity names use them.
    constexpr const char* kDitherNames[NUM_SPHERES_X] = {
        "none", "bayer2", "bayer4", "bayer8", "bayer16", "bluenoise", "ignnoise"
    };

    constexpr const char* kLabels[NUM_SPHERES_X] = {
        "Alpha\nBlend",
        "Bayer2\nDither",
        "Bayer4\nDither",
        "Bayer8\nDither",
        "Bayer16\nDither",
        "Blue-noise\nDither",
        "IGN-noise\nDither"
    };

    // Font size 64 scaled down to 0.3 world units, see the header.
    constexpr int kTextFontSize = 64;
    constexpr float kTextWorldSize = 0.3f;
}

class MaterialTransparencyExample final : public ExampleApp
{
public:
    // Anti-aliasing off, to make the dithering more pronounced
    MaterialTransparencyExample() : ExampleApp({.title = "Material Transparency", .antialias = false}) {}

protected:
    void configure(AppOptions& options) override
    {
        // The labels are world-space text elements, on no screen.
        registerUi(options, {.screen = false});
    }

    bool create() override
    {
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _font = _fontAsset->resourceAs<FontResource>();
        if (!_font) {
            spdlog::error("Failed to load fonts/roboto-regular.json");
            return false;
        }

        // Create an entity with a camera component
        Entity* camera = createCamera(Vector3(0.0f, -0.5f, 16.0f));
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
            cameraComponent->setToneMapping(TONEMAP_LINEAR);
        }

        _blendState = std::make_shared<BlendState>(BlendState::alphaBlend());

        for (int i = 0; i < NUM_SPHERES_X; ++i) {
            for (int j = 0; j < NUM_SPHERES_Z; ++j) {
                createSphere(i, j);
            }
        }

        const float y = (NUM_SPHERES_Z + 1) * -0.5f;
        for (int i = 0; i < NUM_SPHERES_X; ++i) {
            createText(kLabels[i], 1.5f * (static_cast<float>(i) - (NUM_SPHERES_X - 1) * 0.5f), y);
        }

        return true;
    }

private:
    void createSphere(const int x, const int z)
    {
        const std::string suffix = std::string(kDitherNames[x]) + "-" + std::to_string(z);

        auto material = std::make_shared<StandardMaterial>();
        material->setName("material-" + suffix);
        material->setEmissive(Color(1.0f, 0.0f, 0.0f, 1.0f));
        material->setSpecular(Color(1.0f, 1.0f, 1.0f, 1.0f));
        material->setMetalness(0.0f);
        material->setGloss(0.5f);
        material->setUseMetalness(true);

        if (kDitherOptions[x] == DitherMode::DITHER_NONE) {
            // Alpha blending material
            material->setBlendState(_blendState);
            material->setTransparent(true);
        } else {
            // Alpha dithering material
            material->setOpacityDitherMode(kDitherOptions[x]);
        }

        // The spheres should seem to fade out linearly, so the perceived opacity is
        // converted from sRGB to linear space
        const float perceivedOpacity = static_cast<float>(z + 1) / NUM_SPHERES_Z;
        material->setOpacity(std::pow(perceivedOpacity, 2.2f));

        Entity* sphere = createPrimitive("sphere", material.get(),
            Vector3(1.5f * (static_cast<float>(x) - (NUM_SPHERES_X - 1) * 0.5f),
                static_cast<float>(z) - (NUM_SPHERES_Z - 1) * 0.5f, 0.0f),
            Vector3(0.9f, 0.9f, 0.9f));
        sphere->setName("entity-" + suffix);
        _materials.push_back(std::move(material));
    }

    void createText(const std::string& message, const float x, const float y)
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
        text->setLocalPosition(x, y, 0.0f);
        text->setLocalScale(scale, scale, scale);
    }

    std::unique_ptr<Asset> _fontAsset;
    FontResource* _font = nullptr;
    std::shared_ptr<BlendState> _blendState;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
};

VISUTWIN_EXAMPLE_MAIN(MaterialTransparencyExample)
