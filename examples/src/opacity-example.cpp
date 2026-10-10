// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream test/opacity.
//
// A 5 x 5 grid of boxes (scale 0.7, one unit apart, centred on the origin) all turning
// together, at (20 t, 30 t, 0) degrees. Each box has a material of its own: an emissive
// colour of (column, row, 1 - row), the seaside-rocks diffuse as its emissive map, and the
// seaside-rocks roughness map as a separate opacity map read from its RED channel. Culling
// is off, so the back faces show through the holes. The columns raise the alpha test from
// left to right, (column + 1) / 6 - 0.1, and the rows blend bottom to top with ADDITIVE,
// ADDITIVEALPHA, SCREEN, NORMAL and no blending at all; every blended row draws in the
// transparent sublayer and keeps writing depth. Two world-space text labels, "Alpha Test"
// above the grid and "Alpha Blend" turned 90 degrees down its left side, name the axes.
// The boxes stand over a large grey half-metal box as the ground, lit by a white
// shadow-casting directional light at (45, 180, 0), against a dark grey clear colour.
//
// Upstream's example has no control panel, so there are no keys beyond the orbit camera:
//   R reset camera | F1 HUD | Esc quit | LMB/RMB orbit, Shift/MMB pan, Wheel zoom
//
// Upstream's orbit camera starts at (10, 6, 22) and initializes while the scene is still
// empty, so it frames an empty bounding box: it looks at the origin and its framing
// distance is clamped up to its 12-unit minimum. The port starts it there, 12 units from
// the origin along (10, 6, 22).
//
// DEVIATIONS:
// - upstream's orbitCamera script (inertia 0.2, distance 12-100) is the examples'
//   CameraControls in orbit mode, with the same zoom range and the pitch at +/-90 degrees;
//   its damping is CameraControls' own, not the 0.2 s inertia.
// - ElementComponent::setFontSize takes an int, so upstream's font size of 0.5 is font
//   size 64 on an entity scaled by 0.5 / 64: the same size in world units.
//
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
#include "core/math/quaternion.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "platform/graphics/blendState.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int NUM_BOXES = 5;

    // Font size 64 scaled down to 0.5 world units, see the header.
    constexpr int kTextFontSize = 64;
    constexpr float kTextWorldSize = 0.5f;

    enum class BlendType
    {
        Additive,
        AdditiveAlpha,
        Screen,
        Normal,
        None
    };

    // Alpha blend modes for the individual rows, bottom to top.
    constexpr BlendType kRowBlendTypes[NUM_BOXES] = {
        BlendType::Additive, BlendType::AdditiveAlpha, BlendType::Screen, BlendType::Normal, BlendType::None
    };

    /// The blend state of an upstream blend type: the colour factors, the alpha factors
    /// (the colour ones except for NORMAL), and blending off for NONE.
    std::shared_ptr<BlendState> createBlendState(const BlendType type)
    {
        int src = BLENDMODE_ONE;
        int dst = BLENDMODE_ZERO;
        int alphaSrc = -1;
        switch (type) {
        case BlendType::Additive:
            src = BLENDMODE_ONE;
            dst = BLENDMODE_ONE;
            break;
        case BlendType::AdditiveAlpha:
            src = BLENDMODE_SRC_ALPHA;
            dst = BLENDMODE_ONE;
            break;
        case BlendType::Screen:
            src = BLENDMODE_ONE_MINUS_DST_COLOR;
            dst = BLENDMODE_ONE;
            break;
        case BlendType::Normal:
            src = BLENDMODE_SRC_ALPHA;
            dst = BLENDMODE_ONE_MINUS_SRC_ALPHA;
            alphaSrc = BLENDMODE_ONE;
            break;
        case BlendType::None:
            break;
        }

        auto state = std::make_shared<BlendState>();
        state->setColorOp(BLENDEQUATION_ADD);
        state->setColorSrcFactor(src);
        state->setColorDstFactor(dst);
        state->setAlphaOp(BLENDEQUATION_ADD);
        state->setAlphaSrcFactor(alphaSrc >= 0 ? alphaSrc : src);
        state->setAlphaDstFactor(dst);
        state->setEnabled(type != BlendType::None);
        return state;
    }
}

class OpacityExample final: public ExampleApp
{
public:
    OpacityExample(): ExampleApp({.title = "Opacity"}) {}

protected:
    void configure(AppOptions& options) override
    {
        // The labels are world-space text elements, on no screen.
        registerUi(options, {.screen = false});
    }

    bool create() override
    {
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        // Both maps are decoded by what reads them: the emissive map from sRGB, the
        // opacity map not at all.
        _rocksAsset = std::make_unique<Asset>("rocks", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-diffuse-alpha.png"), AssetData{.mipmaps = true});
        _opacityAsset = std::make_unique<Asset>("opacity", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-roughness.jpg"), AssetData{.mipmaps = true});
        _font = _fontAsset->resourceAs<FontResource>();
        Texture* rocks = _rocksAsset->resourceAs<Texture>();
        Texture* opacity = _opacityAsset->resourceAs<Texture>();
        if (!_font || !rocks || !opacity) {
            spdlog::error("Failed to load fonts/roboto-regular.json or the seaside-rocks textures");
            return false;
        }

        // Create an entity with a camera component
        const Vector3 cameraDirection = Vector3(10.0f, 6.0f, 22.0f).normalized();
        Entity* camera = createCamera(cameraDirection * 12.0f);
        camera->lookAt(0.0f, 0.0f, 0.0f);
        if (auto* cameraComp = camera->findComponent<CameraComponent>()) {
            cameraComp->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        }

        // Orbit the origin with the mouse and touch
        if (CameraControls* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(12.0f, 100.0f));
            controls->storeResetState();
        }

        for (int row = 0; row < NUM_BOXES; ++row) {
            _blendStates.push_back(createBlendState(kRowBlendTypes[row]));
        }
        for (int i = 0; i < NUM_BOXES; ++i) {
            for (int j = 0; j < NUM_BOXES; ++j) {
                _boxes.push_back(createBox(j, i, 0, rocks, opacity));
            }
        }

        createText("Alpha Test", 0.0f, (NUM_BOXES + 1) * 0.5f, 0.0f, 0.0f);
        createText("Alpha Blend", -(NUM_BOXES + 1) * 0.5f, 0.0f, 0.0f, 90.0f);

        // Ground
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.5f, 1.0f));
        _groundMaterial->setGloss(0.4f);
        _groundMaterial->setMetalness(0.5f);
        _groundMaterial->setUseMetalness(true);
        createPrimitive("box", _groundMaterial.get(), Vector3(0.0f, -3.0f, 0.0f), Vector3(30.0f, 1.0f, 30.0f));

        // Light
        Entity* light = createDirectionalLight(Vector3(45.0f, 180.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowDistance(20.0f);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
            lightComp->setShadowResolution(2048);
        }

        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Rotate the boxes
        const Quaternion rotation = Quaternion::fromEulerAngles(20.0f * _time, 30.0f * _time, 0.0f);
        for (Entity* box : _boxes) {
            box->setRotation(rotation);
        }
    }

private:
    Entity* createBox(const int x, const int y, const int z, Texture* emissiveMap, Texture* opacityMap)
    {
        const auto fx = static_cast<float>(x);
        const auto fy = static_cast<float>(y);
        auto material = std::make_shared<StandardMaterial>();

        // Alpha test value. The mode goes first: it resets the blend and depth state.
        material->setAlphaMode(AlphaMode::MASK);
        material->setAlphaCutoff((fx + 1.0f) / (NUM_BOXES + 1) - 0.1f);

        // Emissive colour and texture
        material->setEmissive(Color(fx, fy, 1.0f - fy, 1.0f));
        material->setEmissiveMap(emissiveMap);

        // Opacity map - a separate texture, read from its red channel
        material->setOpacityMap(opacityMap);
        material->setOpacityMapChannel(MapChannel::MAP_CHANNEL_R);

        // Disable culling to see back faces as well
        material->setCullMode(CullMode::CULLFACE_NONE);

        // Alpha blend mode; a blended material sorts with the transparent draws
        const std::shared_ptr<BlendState>& blendState = _blendStates[y];
        material->setBlendState(blendState);
        material->setTransparent(blendState->enabled());

        _materials.push_back(material);
        return createPrimitive("box", material.get(),
            Vector3(fx - (NUM_BOXES - 1) * 0.5f, fy - (NUM_BOXES - 1) * 0.5f, static_cast<float>(z)),
            Vector3(0.7f, 0.7f, 0.7f));
    }

    void createText(const std::string& message, const float x, const float y, const float z, const float rot)
    {
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

    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _rocksAsset;
    std::unique_ptr<Asset> _opacityAsset;
    FontResource* _font = nullptr;

    std::vector<std::shared_ptr<BlendState>> _blendStates;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::vector<Entity*> _boxes;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(OpacityExample)
