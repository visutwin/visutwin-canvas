// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream materials/material-physical.
//
// A 5 x 5 grid of spheres (scale 0.9, one unit apart, centred on the origin), each with a
// metalness-workflow material of its own: the gloss rises from 0 to 1 left to right and the
// metalness from 0 to 1 bottom to top. Two world-space text labels, "Glossiness" below the
// grid and "Metalness" turned 90 degrees down its left side, name the axes. The scene is lit
// by the helipad environment atlas alone (no light), drawn as the skybox at mip 1, and seen
// by a fixed camera at (0, 0, 9) with ACES tone mapping.
//
// Dragging with the left mouse button held rotates the skybox, and with it the environment
// lighting: 0.2 degrees of pitch per point moved down and 0.2 degrees of yaw per point moved
// right.
//   F1 HUD | Esc quit | LMB drag rotate the skybox
//
// DEVIATION: ElementComponent::setFontSize takes an int, so upstream's font size of 0.5 is
// font size 64 on an entity scaled by 0.5 / 64: the same size in world units.
//
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
#include "core/math/quaternion.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "platform/input/mouse.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int NUM_SPHERES = 5;

    // Font size 64 scaled down to 0.5 world units, see the header.
    constexpr int kTextFontSize = 64;
    constexpr float kTextWorldSize = 0.5f;
}

class MaterialPhysicalExample final : public ExampleApp
{
public:
    MaterialPhysicalExample() : ExampleApp({.title = "Material Physical"}) {}

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

        // Create an entity with a camera component
        Entity* camera = createCamera(Vector3(0.0f, 0.0f, 9.0f));
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->setToneMapping(TONEMAP_ACES);
        }

        for (int i = 0; i < NUM_SPHERES; ++i) {
            for (int j = 0; j < NUM_SPHERES; ++j) {
                createSphere(j, i, 0);
            }
        }

        createText("Glossiness", 0.0f, -(NUM_SPHERES + 1) * 0.5f, 0.0f, 0.0f);
        createText("Metalness", -(NUM_SPHERES + 1) * 0.5f, 0.0f, 0.0f, 90.0f);

        return true;
    }

    void update(float /*dt*/) override
    {
        // Rotate the skybox using mouse input, while the left button is held
        Mouse* mouse = engine()->mouse();
        if (!mouse || !mouse->isPressed(MouseButton::Left)) {
            return;
        }
        const float dx = mouse->deltaX();
        const float dy = mouse->deltaY();
        if (dx == 0.0f && dy == 0.0f) {
            return;
        }
        _x += dx;
        _y += dy;
        scene()->setSkyboxRotation(Quaternion::fromEulerAngles(0.2f * _y, 0.2f * _x, 0.0f));
    }

private:
    void createSphere(const int x, const int y, const int z)
    {
        const auto fx = static_cast<float>(x);
        const auto fy = static_cast<float>(y);

        auto material = std::make_shared<StandardMaterial>();
        material->setMetalness(fy / (NUM_SPHERES - 1));
        material->setGloss(fx / (NUM_SPHERES - 1));
        material->setUseMetalness(true);

        createPrimitive("sphere", material.get(),
            Vector3(fx - (NUM_SPHERES - 1) * 0.5f, fy - (NUM_SPHERES - 1) * 0.5f, static_cast<float>(z)),
            Vector3(0.9f, 0.9f, 0.9f));
        _materials.push_back(std::move(material));
    }

    void createText(const std::string& message, const float x, const float y, const float z, const float rot)
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
        text->setLocalEulerAngles(0.0f, 0.0f, rot);
        text->setLocalScale(scale, scale, scale);
    }

    std::unique_ptr<Asset> _helipadAsset;
    std::unique_ptr<Asset> _fontAsset;
    FontResource* _font = nullptr;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;

    // The accumulated mouse movement while dragging, in points
    float _x = 0.0f;
    float _y = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(MaterialPhysicalExample)
