// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream camera-frame/hdr.
//
// The apartment (scaled 30x) under the helipad env atlas (exposure 1.2), with the neon "love"
// sign at emissive intensity 200 blooming over an HDR scene. The camera frame renders 4x MSAA,
// ACES tone mapping, bloom 0.03 over 7 levels, a vignette (0.5, 1, 0.5, 0.5) and the blue colour
// LUT at full intensity; a colour chart drawn by a screen-space image element in the bottom-right
// corner shows the LUT does not touch the UI, which renders after the composition.
//
// Keys stand in for upstream's controls panel, over the same parameters:
//   H      the camera frame on / off (off renders the scene straight to the window)
//   T      tone mapping, cycled through linear, filmic, ACES, ACES2, neutral, AgX, AgX punchy, none
//   - / =  colour LUT intensity -/+ 0.1
//   B      bloom on / off          [ / ]  bloom intensity -/+ 0.01
//   , / .  bloom blur level -/+ 1  ; / '  bloom threshold -/+ 0.5
//   Q      high-quality bloom      D      the bloom debug view (the bloom on its own)
//
// DEVIATIONS:
// - upstream's CameraControls script (focus (-80, 80, -20)) is the examples' CameraControls in
//   orbit mode around that point from the authored position. Its PerspectiveCorrection script
//   is not ported; its vertical correction starts at 0, which changes nothing.
// - the colour LUT is loaded non-sRGB and decoded in the compose shader (as every colour LUT
//   here). The colour chart is upstream's colors.webp, decoded losslessly to colors.png.
//
#include <algorithm>
#include <array>
#include <memory>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "../uiElements.h"
#include "framework/assets/asset.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    template <typename Fn>
    void forEachMaterial(Entity* entity, Fn&& fn)
    {
        if (!entity) {
            return;
        }
        for (auto* render : entity->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                if (auto* material = meshInstance ? meshInstance->material() : nullptr) {
                    fn(material);
                }
            }
        }
    }

    constexpr std::array<int, 8> kToneMappings = {TONEMAP_LINEAR, TONEMAP_FILMIC, TONEMAP_ACES, TONEMAP_ACES2,
        TONEMAP_NEUTRAL, TONEMAP_AGX, TONEMAP_AGX_PUNCHY, TONEMAP_NONE};
}

class HdrExample final : public ExampleApp
{
public:
    // The scene is rendered to an antialiased texture, so the back buffer is not.
    HdrExample() : ExampleApp({.title = "HDR Example", .antialias = false}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _envAtlas = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _apartment = std::make_unique<Asset>("apartment", AssetType::CONTAINER, assetPath("models/apartment.glb"));
        _love = std::make_unique<Asset>("love", AssetType::CONTAINER, assetPath("models/love.glb"));
        _colors = std::make_unique<Asset>("colors", AssetType::TEXTURE, assetPath("textures/colors.png"));
        _colorLut = std::make_unique<Asset>("colorLut", AssetType::TEXTURE, assetPath("cube-luts/lut-blue.png"),
            AssetData{.mipmaps = false});

        // Setup skydome with low intensity
        if (Texture* envAtlas = _envAtlas->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        }
        scene()->setExposure(1.2f);

        auto* apartmentContainer = _apartment->resourceAs<ContainerResource>();
        auto* loveContainer = _love->resourceAs<ContainerResource>();
        if (!apartmentContainer || !loveContainer) {
            spdlog::error("hdr needs models/apartment.glb and models/love.glb");
            return false;
        }
        auto* apartment = apartmentContainer->instantiateRenderEntity();
        apartment->setLocalScale(30.0f, 30.0f, 30.0f);
        root()->addChild(apartment);

        // The love sign, its neon tube emissive enough to bloom
        auto* love = loveContainer->instantiateRenderEntity();
        love->setLocalPosition(-80.0f, 30.0f, -20.0f);
        love->setLocalScale(130.0f, 130.0f, 130.0f);
        love->rotate(0.0f, -90.0f, 0.0f);
        root()->addChild(love);
        if (auto* neon = dynamic_cast<Entity*>(love->findByName("s.0009_Standard_FF00BB_0"))) {
            if (auto* render = neon->findComponent<RenderComponent>(); render && !render->meshInstances().empty()) {
                if (auto* standard = dynamic_cast<StandardMaterial*>(render->meshInstances()[0]->material())) {
                    standard->setEmissiveIntensity(200.0f);
                }
            }
        }
        forEachMaterial(love, [](Material* material) {
            if (auto* standard = dynamic_cast<StandardMaterial*>(material)) {
                standard->setUseDynamicRefraction(false);
            }
        });

        // The camera
        auto* camera = createCamera(Vector3(-50.0f, 100.0f, 220.0f));
        _cameraComp = camera->findComponent<CameraComponent>();
        if (!_cameraComp || !_cameraComp->camera()) {
            return false;
        }
        _cameraComp->camera()->setFarClip(1500.0f);
        _cameraComp->camera()->setFov(80.0f);
        if (auto* controls = addOrbitControls(camera, Vector3(-80.0f, 80.0f, -20.0f))) {
            controls->setZoomRange(Vector2(10.0f, 500.0f));
            controls->setMoveSpeed(60.0f);
            controls->setMoveFastSpeed(180.0f);
            controls->storeResetState();
        }

        // A colour chart in the bottom-right corner, drawn over the composition
        if (Texture* colors = _colors->resourceAs<Texture>()) {
            ScreenComponent* screen = createScreen();
            const float w = static_cast<float>(colors->width());
            const float h = static_cast<float>(colors->height());
            ElementComponent* chart = createElement(engine(), screen->entity(),
                {.type = ElementType::Image, .texture = colors, .anchor = Vector4(1.0f, 0.0f, 1.0f, 0.0f),
                 .pivot = Vector2(1.0f, 0.0f), .width = w * 0.5f, .height = h * 0.5f});
            chart->entity()->setLocalPosition(-0.1f * w, 0.1f * h, 0.0f);
        }

        // The camera frame
        _rendering = _cameraComp->rendering();
        _rendering.samples = 4;
        _rendering.bloomIntensity = 0.03f;
        _rendering.bloomBlurLevel = 7;
        _rendering.vignetteEnabled = true;
        _rendering.vignetteInner = 0.5f;
        _rendering.vignetteOuter = 1.0f;
        _rendering.vignetteCurvature = 0.5f;
        _rendering.vignetteIntensity = 0.5f;
        _rendering.colorLUT = _colorLut->resourceAs<Texture>();
        _rendering.colorLUTIntensity = 1.0f;
        apply();

        spdlog::info("Controls: H frame, T tone mapping, - = LUT, B bloom, [ ] intensity, , . blur, ; ' threshold, "
            "Q high quality, D bloom debug");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_H: _frameEnabled = !_frameEnabled; break;
        case SDLK_T: _toneMapping = (_toneMapping + 1) % kToneMappings.size(); break;
        case SDLK_MINUS: _rendering.colorLUTIntensity = std::max(0.0f, _rendering.colorLUTIntensity - 0.1f); break;
        case SDLK_EQUALS: _rendering.colorLUTIntensity = std::min(1.0f, _rendering.colorLUTIntensity + 0.1f); break;
        case SDLK_B: _bloomEnabled = !_bloomEnabled; break;
        case SDLK_LEFTBRACKET: _bloomIntensity = std::max(0.0f, _bloomIntensity - 0.01f); break;
        case SDLK_RIGHTBRACKET: _bloomIntensity = std::min(0.1f, _bloomIntensity + 0.01f); break;
        case SDLK_COMMA: _rendering.bloomBlurLevel = std::max(1, _rendering.bloomBlurLevel - 1); break;
        case SDLK_PERIOD: _rendering.bloomBlurLevel = std::min(16, _rendering.bloomBlurLevel + 1); break;
        case SDLK_SEMICOLON: _rendering.bloomThreshold = std::max(0.0f, _rendering.bloomThreshold - 0.5f); break;
        case SDLK_APOSTROPHE: _rendering.bloomThreshold = std::min(10.0f, _rendering.bloomThreshold + 0.5f); break;
        case SDLK_Q: _rendering.bloomHighQuality = !_rendering.bloomHighQuality; break;
        case SDLK_D: _rendering.composeDebug = _rendering.composeDebug.empty() ? "bloom" : ""; break;
        default: return false;
        }
        apply();
        return true;
    }

private:
    // The camera frame turned off renders the scene straight to the window: nothing it needs
    // a frame for remains.
    void apply()
    {
        _cameraComp->setToneMapping(kToneMappings[_toneMapping]);
        _rendering.bloomIntensity = _bloomEnabled ? _bloomIntensity : 0.0f;
        if (_frameEnabled) {
            _cameraComp->setRendering(_rendering);
        } else {
            RenderingSettings off;
            _cameraComp->setRendering(off);
        }
        spdlog::info("frame {} tone mapping {} LUT {:.1f} bloom {} {:.2f} blur {} threshold {:.1f} hq {} debug '{}'",
            _frameEnabled, kToneMappings[_toneMapping], _rendering.colorLUTIntensity, _bloomEnabled, _bloomIntensity,
            _rendering.bloomBlurLevel, _rendering.bloomThreshold, _rendering.bloomHighQuality, _rendering.composeDebug);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _apartment;
    std::unique_ptr<Asset> _love;
    std::unique_ptr<Asset> _colors;
    std::unique_ptr<Asset> _colorLut;
    CameraComponent* _cameraComp = nullptr;
    RenderingSettings _rendering;
    bool _frameEnabled = true;
    bool _bloomEnabled = true;
    float _bloomIntensity = 0.03f;
    size_t _toneMapping = 2;   // ACES
};

VISUTWIN_EXAMPLE_MAIN(HdrExample)
