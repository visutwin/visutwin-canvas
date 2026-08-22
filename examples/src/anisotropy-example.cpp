// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Anisotropic specular demo (mirrors upstream materials/material-anisotropic).
//
// An 11 x 6 grid of metallic procedural spheres lit by the helipad env atlas plus
// a single directional light, matching upstream's sweeps exactly:
//   * X axis (columns): anisotropy 0 -> 1 (x / (NUM_SPHERES_X - 1)). At 0 the
//     highlight is a round GGX blob; as it grows the highlight stretches into a
//     brushed-metal streak.
//   * Z axis (rows): gloss 0 -> 1 (z / (NUM_SPHERES_Z - 1)) — upstream labels this
//     axis "Roughness" even though it sets gloss.
//
// Upstream also sets material.enableGGXSpecular; this engine has no equivalent
// flag because the anisotropic path is gated automatically on a non-zero
// anisotropy value (programLibrary.cpp: options.anisotropy = anisotropy() != 0),
// so the leftmost column renders isotropic exactly as upstream's does.
//
// The two axis labels ("Anisotropy", "Roughness") lie flat on the ground plane as
// WORLD-SPACE text: a Text element with no ScreenComponent ancestor is parented to
// its entity and drawn on the world layer with depth testing, so it follows the
// entity's full transform. setFontSize takes an int, so the em size is set coarsely
// (64) and scaled down on the entity; the element's HEIGHT must equal fontSize or
// the line is parked half a box-height above the origin.
//
// Orbit camera (CameraControls) starts at the upstream camera pose.
//
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

// Grid dimensions and spacing match upstream material-anisotropic exactly.
constexpr int NUM_SPHERES_X = 11;  // anisotropy 0 .. 1
constexpr int NUM_SPHERES_Z = 6;   // gloss 0 .. 1
constexpr float SPACING = 1.0f;

class AnisotropyExample final: public ExampleApp
{
public:
    AnisotropyExample(): ExampleApp({.title = "Anisotropic Specular"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ElementComponentSystem>();
        _elementInput = std::make_shared<ElementInput>();
        options.elementInput = _elementInput;
    }

    bool create() override
    {
        // Upstream sets only these — no ambient, no exposure or skybox-intensity
        // overrides — so the env atlas alone lights the spheres.
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setSkyboxMip(1);

        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );

        const auto helipadResource = _helipad->resource();
        if (!helipadResource) {
            spdlog::error("Failed to load helipad env atlas");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));

        // Single directional light (as upstream: base euler +90 X, -75 Y).
        createDirectionalLight(Vector3(90.0f, -75.0f, 0.0f));

        _materials.reserve(NUM_SPHERES_X * NUM_SPHERES_Z);

        for (int iz = 0; iz < NUM_SPHERES_Z; ++iz) {
            // gloss = z / (NUM_SPHERES_Z - 1), i.e. 0 .. 1 (upstream's "Roughness" axis).
            const float gloss = NUM_SPHERES_Z > 1
                ? static_cast<float>(iz) / static_cast<float>(NUM_SPHERES_Z - 1)
                : 0.0f;

            for (int ix = 0; ix < NUM_SPHERES_X; ++ix) {
                // anisotropy = x / (NUM_SPHERES_X - 1), i.e. 0 .. 1 (upstream's sweep).
                const float aniso = NUM_SPHERES_X > 1
                    ? static_cast<float>(ix) / static_cast<float>(NUM_SPHERES_X - 1)
                    : 0.0f;

                auto material = std::make_shared<StandardMaterial>();
                material->setUseMetalness(true);
                material->setMetalness(1.0f);
                material->setGloss(gloss);
                material->setAnisotropy(aniso);
                _materials.push_back(material);

                auto* sphere = new Entity();
                sphere->setEngine(engine());
                sphere->setLocalPosition(
                    (static_cast<float>(ix) - (NUM_SPHERES_X - 1) * 0.5f) * SPACING,
                    0.0f,
                    (static_cast<float>(iz) - (NUM_SPHERES_Z - 1) * 0.5f) * SPACING
                );
                sphere->setLocalScale(0.7f, 0.7f, 0.7f);
                if (auto* render = static_cast<RenderComponent*>(sphere->addComponent<RenderComponent>())) {
                    render->setMaterial(material.get());
                    render->setType("sphere");
                }
                root()->addChild(sphere);
            }
        }

        createAxisLabels();

        // Camera pose copied from upstream: translate(0, 9, 9) + rotate(-48, 0, 0).
        // CameraControls derives its orbit state FROM this pose rather than moving
        // the camera, so the default framing matches while orbiting still works.
        // (Upstream's -48 deg pitch is ~3 deg off looking straight at the origin;
        // the controls settle on the exact look-at, which is visually identical.)
        auto* camera = createCamera(Vector3(0.0f, 9.0f, 9.0f), Vector3(-48.0f, 0.0f, 0.0f));

        auto* cameraControls = addOrbitControls(camera, _focusPoint);
        cameraControls->setMoveSpeed(2.0f * _gridRadius);
        cameraControls->setMoveFastSpeed(4.0f * _gridRadius);
        cameraControls->setMoveSlowSpeed(_gridRadius);
        cameraControls->storeResetState();
        _controls = cameraControls;

        spdlog::info("Anisotropic specular: {}x{} metallic sphere grid (upstream material-anisotropic parity).", NUM_SPHERES_X, NUM_SPHERES_Z);
        spdlog::info("Columns sweep anisotropy 0 -> 1; rows sweep gloss 0 -> 1.");
        spdlog::info("Watch the round GGX highlight stretch into a brushed-metal streak toward the right.");
        spdlog::info("Orbit: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset, Esc quit.");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && _controls) {
            _controls->focus(_focusPoint, std::max(_gridRadius * 1.6f, 6.0f));
            return true;
        }
        return false;
    }

    void preRender() override
    {
        _elementInput->syncTextElements();
    }

private:
    // Upstream's two axis labels, lying flat on the ground plane. These are
    // WORLD-SPACE text: the entities have no ScreenComponent ancestor, so the
    // element system parents the glyph mesh to the entity and renders it on the
    // world layer with depth testing, following the entity's full transform.
    //
    // setFontSize takes an int (min 1), so upstream's 0.5 em is expressed as
    // fontSize 1 on an entity scaled by 0.5 — the same size in world units.
    // Text is laid out from `yTop = (1 - pivot.y) * height` and flows DOWN, so a
    // single line is centred on the entity origin only when height == fontSize.
    // Leaving the default height (50) parks the label 25 mesh units above the
    // scene. Width is irrelevant here: with centre pivot the x offset works out
    // to -lineWidth/2 regardless, and wrapping is off.
    //
    // fontSize is an int, so size is set coarsely and scaled down: 64 mesh units
    // per em on a 0.5/64-scaled entity gives upstream's 0.5 world units per em.
    void createAxisLabels()
    {
        constexpr int LABEL_FONT_SIZE = 64;
        constexpr float LABEL_EM_WORLD = 0.5f;
        constexpr float LABEL_SCALE = LABEL_EM_WORLD / static_cast<float>(LABEL_FONT_SIZE);

        // Bitmap font for the two world-space axis labels (upstream uses arial.json too).
        _labelFont = std::make_unique<Asset>(
            "arial-font",
            AssetType::FONT,
            assetPath("fonts/arial.json")
        );

        FontResource* labelFontResource = nullptr;
        if (const auto fontRes = _labelFont->resource();
            fontRes.has_value() && std::holds_alternative<FontResource*>(*fontRes)) {
            labelFontResource = std::get<FontResource*>(*fontRes);
        }
        if (!labelFontResource) {
            spdlog::warn("Label font failed to load — axis labels will be missing");
            return;
        }

        const auto createLabel = [&](const std::string& text, const Vector3& position,
                                     const Vector3& eulerAngles) {
            auto* label = new Entity();
            label->setEngine(engine());
            if (auto* element = static_cast<ElementComponent*>(
                    label->addComponent<ElementComponent>())) {
                element->setType(ElementType::Text);
                element->setText(text);
                element->setFontResource(labelFontResource);
                element->setFontSize(LABEL_FONT_SIZE);
                element->setHeight(static_cast<float>(LABEL_FONT_SIZE));
                element->setWidth(static_cast<float>(LABEL_FONT_SIZE) * 16.0f);
                element->setPivot(Vector2(0.5f, 0.5f));
                element->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            }
            label->setLocalPosition(position);
            label->setLocalEulerAngles(eulerAngles.getX(), eulerAngles.getY(),
                eulerAngles.getZ());
            label->setLocalScale(LABEL_SCALE, LABEL_SCALE, LABEL_SCALE);
            root()->addChild(label);
        };

        createLabel("Anisotropy",
            Vector3(0.0f, 0.0f, (NUM_SPHERES_Z + 1) * 0.5f * SPACING),
            Vector3(-90.0f, 0.0f, 0.0f));
        createLabel("Roughness",
            Vector3(-(NUM_SPHERES_X + 1) * 0.5f * SPACING, 0.0f, 0.0f),
            Vector3(-90.0f, 90.0f, 0.0f));
    }

    const Vector3 _focusPoint{0.0f, 0.0f, 0.0f};
    const float _gridRadius = std::max(NUM_SPHERES_X, NUM_SPHERES_Z) * SPACING * 0.5f;

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _labelFont;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    CameraControls* _controls = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(AnisotropyExample)
