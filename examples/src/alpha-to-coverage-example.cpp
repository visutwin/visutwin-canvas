// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream test/alpha-to-coverage (a hidden test example).
//
// Material::setAlphaToCoverage. One opacity ramp (0.15, 0.35, 0.55, 0.75, 0.95) in three rows of
// spheres: on top alpha to coverage (no blending, the alpha taken by the multisample coverage mask
// instead), in the middle alpha blending (no depth writes), at the bottom opaque. Each sphere is
// an orange metal (diffuse (0.9, 0.35, 0.2), metalness 0.1, gloss 0.6), 1.1 across and 1.3 apart,
// the rows at y 1.9, 0 and -1.9, each labelled 0.95 above it (Roboto, 0.32 high). Behind them a
// 24 x 14 checkerboard (checkboard.png tiled 8 x 5, gloss 0, specular workflow) at z = -3, so
// anything showing through a sphere is plain. Ambient 0.4, no shadows, a directional light of
// 1.5 turned (45, 20, 0); the camera at (0, 1, 11) looks at (0, 1, 0) and clears to
// (0.05, 0.06, 0.08). Alpha to coverage needs a multisampled target: with MSAA off the top row
// draws as the opaque one does.
//
// Keys standing in for upstream's controls panel:
//   1  alpha to coverage on the top row on / off (on to begin with)
//   2  render through a camera frame (into a texture) instead of the back buffer (off)
//   3  MSAA on / off (on): the camera frame's sample count, 4 or 1
//
// DEVIATIONS:
// - The back buffer's sample count is fixed when the device is created (4x here, as upstream
//   starts), and nothing changes it at run time: the MSAA key acts on the camera frame alone, so
//   with the back buffer it does nothing. Upstream switches the back buffer by poking its
//   device's internal state. VISUTWIN_ANTIALIAS=0 starts with a single-sampled back buffer.
// - No Format control: the camera frame chooses its scene format itself.
// - ElementComponent::setFontSize takes an int, so the labels are laid out at 64 and scaled
//   down to upstream's 0.32 units.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/element/elementComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr std::array<float, 5> kOpacities = {0.15f, 0.35f, 0.55f, 0.75f, 0.95f};

    // The labels are laid out at this font size and scaled to upstream's 0.32 units.
    constexpr int kLabelFontSize = 64;
    constexpr float kLabelWorldSize = 0.32f;

    enum class RowMode
    {
        Coverage,
        Blend,
        Opaque
    };
}

class AlphaToCoverageExample final : public ExampleApp
{
public:
    AlphaToCoverageExample() : ExampleApp({.title = "Alpha To Coverage"}) {}

protected:
    void configure(AppOptions& options) override
    {
        // The labels are world-space text elements, on no screen.
        registerUi(options, {.screen = false});
    }

    bool create() override
    {
        _checkerboardAsset = std::make_unique<Asset>("checkerboard", AssetType::TEXTURE,
            assetPath("textures/checkboard.png"));
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        Texture* checkerboard = _checkerboardAsset->resourceAs<Texture>();
        _font = _fontAsset->resourceAs<FontResource>();
        if (!checkerboard || !_font) {
            spdlog::error("Failed to load textures/checkboard.png or fonts/roboto-regular.json");
            return false;
        }

        scene()->setAmbientLight(0.4f, 0.4f, 0.4f);
        scene()->lighting().shadowsEnabled = false;

        // A checkerboard backdrop, so anything showing through the spheres is obvious
        auto backdropMaterial = std::make_shared<StandardMaterial>();
        backdropMaterial->setDiffuseMap(checkerboard);
        backdropMaterial->setDiffuseMapTiling(Vector2(8.0f, 5.0f));
        backdropMaterial->setGloss(0.0f);
        backdropMaterial->setUseMetalness(false);
        Entity* backdrop = createPrimitive("plane", backdropMaterial.get(), Vector3(0.0f, 0.0f, -3.0f),
            Vector3(24.0f, 1.0f, 14.0f));
        backdrop->setName("Backdrop");
        backdrop->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
        _materials.push_back(backdropMaterial);

        createRow(1.9f, RowMode::Coverage, "Alpha To Coverage");
        createRow(0.0f, RowMode::Blend, "Alpha Blend");
        createRow(-1.9f, RowMode::Opaque, "Opaque");

        createDirectionalLight(Vector3(45.0f, 20.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.5f);

        Entity* camera = createCamera(Vector3(0.0f, 1.0f, 11.0f));
        camera->lookAt(Vector3(0.0f, 1.0f, 0.0f));
        _camera = camera->findComponent<CameraComponent>();
        _camera->camera()->setClearColor(Color(0.05f, 0.06f, 0.08f, 1.0f));

        applySettings();
        spdlog::info("Keys: 1 alpha to coverage, 2 camera frame, 3 MSAA");
        return true;
    }

    void update(float /*dt*/) override
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        if (keyboard->wasPressed(Key::Digit1)) {
            _alphaToCoverage = !_alphaToCoverage;
        } else if (keyboard->wasPressed(Key::Digit2)) {
            _cameraFrame = !_cameraFrame;
        } else if (keyboard->wasPressed(Key::Digit3)) {
            _msaa = !_msaa;
        } else {
            return;
        }
        applySettings();
    }

private:
    void createLabel(const std::string& message, const float y)
    {
        ElementComponent* element = createElement(engine(), root(), {
            .type = ElementType::Text,
            .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
            .pivot = Vector2(0.5f, 0.5f),
            .font = _font,
            .text = message,
            .fontSize = kLabelFontSize
        });
        Entity* label = element->entity();
        label->setName("Label-" + message);
        constexpr float scale = kLabelWorldSize / static_cast<float>(kLabelFontSize);
        label->setLocalPosition(0.0f, y, 0.0f);
        label->setLocalScale(scale, scale, scale);
    }

    void createRow(const float rowY, const RowMode mode, const std::string& label)
    {
        createLabel(label, rowY + 0.95f);

        for (size_t index = 0; index < kOpacities.size(); ++index) {
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(0.9f, 0.35f, 0.2f, 1.0f));
            material->setUseMetalness(true);
            material->setMetalness(0.1f);
            material->setGloss(0.6f);
            material->setOpacity(kOpacities[index]);

            if (mode == RowMode::Coverage) {
                // No blending: the alpha is consumed by the MSAA coverage mask instead
                material->setAlphaToCoverage(true);
                _coverageMaterials.push_back(material.get());
            } else if (mode == RowMode::Blend) {
                material->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
                material->setTransparent(true);
                auto depth = std::make_shared<DepthState>();
                depth->setDepthWrite(false);
                material->setDepthState(depth);
            }

            const float x = (static_cast<float>(index) - (kOpacities.size() - 1) * 0.5f) * 1.3f;
            Entity* sphere = createPrimitive("sphere", material.get(), Vector3(x, rowY, 0.0f),
                Vector3(1.1f, 1.1f, 1.1f));
            sphere->setName("Sphere-" + std::to_string(static_cast<int>(mode)) + "-" + std::to_string(index));
            _materials.push_back(material);
        }
    }

    void applySettings()
    {
        for (StandardMaterial* material : _coverageMaterials) {
            material->setAlphaToCoverage(_alphaToCoverage);
        }

        // The MSAA control drives the camera frame's samples; the back buffer keeps its own.
        // Samples above 1 alone would send the camera through a frame, so the back buffer path
        // asks for 1.
        auto rendering = _camera->rendering();
        rendering.useCameraFrame = _cameraFrame;
        rendering.samples = _cameraFrame && _msaa ? 4 : 1;
        _camera->setRendering(rendering);

        spdlog::info("Alpha to coverage {}, rendering to {}, MSAA {}", _alphaToCoverage ? "on" : "off",
            _cameraFrame ? "texture" : "back buffer", _cameraFrame ? (_msaa ? "4x" : "off") : "back buffer's");
    }

    std::unique_ptr<Asset> _checkerboardAsset;
    std::unique_ptr<Asset> _fontAsset;
    FontResource* _font = nullptr;
    CameraComponent* _camera = nullptr;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<StandardMaterial*> _coverageMaterials;

    bool _alphaToCoverage = true;
    bool _cameraFrame = false;
    bool _msaa = true;
};

VISUTWIN_EXAMPLE_MAIN(AlphaToCoverageExample)
