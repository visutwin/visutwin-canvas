// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.07.2026
//
// Port of upstream graphics/post-processing.
//
// A baked sci-fi platform stage lit by a warm directional light, with the
// mosquito-in-amber model slowly turning above it (its amber is a transmissive,
// dynamically refracting material, so the camera frame grabs the scene colour)
// and three emissive boxes pulsing in size so their bloom breathes. The skydome
// is disabled; the helipad atlas only provides IBL and the camera clears to a
// bright HDR tint of the light colour. Two text labels sit on the screen.
//
// Initial settings: render scale 1.8, background 6, emissive 200, ACES, bloom on at
// intensity 5 (-> 0.005) with blur level 16, and grading, colour enhance,
// vignette, fringing and TAA all off.
//
// Keys stand in for the control panel (listed at startup).
//
// @credit
// title: Real-time Refraction Demo: Mosquito in Amber
// author: Sketchfab
// source: https://sketchfab.com/3d-models/real-time-refraction-demo-mosquito-in-amber-37233d6ed84844fea1ebe88069ea58d1
// license: CC BY 4.0 (http://creativecommons.org/licenses/by/4.0/)
//
// @credit
// title: Scifi Platform Stage Scene (Baked)
// author: Sketchfab
// source: https://sketchfab.com/3d-models/scifi-platform-stage-scene-baked-64adb59a716d43e5a8705ff6fe86c0ce
// license: CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/)
//
// DEVIATIONS from upstream:
//  - The control panel is keys. The `debug` select (bloom / vignette / scene
//    views of the camera frame) is not available: the camera frame has no debug
//    output mode here.
//  - "enabled = false" cannot remove the camera frame directly; the E key
//    switches every compose setting off (scale 1, no bloom, no TAA) so the
//    camera falls back to the plain forward path, which is what upstream's
//    disabled CameraFrame renders.
//  - The orbit camera script is CameraControls in orbit mode, aimed at the
//    mosquito's bounds centre, with the zoom range capped at a distanceMax of 190.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kPi = 3.14159265358979f;

    // The panel's initial values.
    struct Settings
    {
        bool enabled = true;
        float scale = 1.8f;
        float background = 6.0f;
        float emissive = 200.0f;
        int tonemapping = TONEMAP_ACES;

        bool bloomEnabled = true;
        float bloomIntensity = 5.0f;
        int bloomBlurLevel = 16;

        bool gradingEnabled = false;
        float gradingSaturation = 1.0f;
        float gradingBrightness = 1.0f;
        float gradingContrast = 1.0f;

        bool colorEnhanceEnabled = false;
        float colorEnhanceShadows = 0.0f;
        float colorEnhanceHighlights = 0.0f;
        float colorEnhanceMidtones = 0.0f;
        float colorEnhanceVibrance = 0.0f;
        float colorEnhanceDehaze = 0.0f;

        bool vignetteEnabled = false;
        float vignetteInner = 0.5f;
        float vignetteOuter = 1.0f;
        float vignetteCurvature = 0.5f;
        float vignetteIntensity = 0.3f;

        bool fringingEnabled = false;
        float fringingIntensity = 50.0f;

        bool taaEnabled = false;
        float taaJitter = 1.0f;
    };

    const char* tonemapName(const int mode)
    {
        switch (mode) {
        case TONEMAP_LINEAR: return "LINEAR";
        case TONEMAP_FILMIC: return "FILMIC";
        case TONEMAP_HEJL: return "HEJL";
        case TONEMAP_ACES: return "ACES";
        case TONEMAP_ACES2: return "ACES2";
        case TONEMAP_NEUTRAL: return "NEUTRAL";
        default: return "?";
        }
    }
}

class PostProcessingExample final: public ExampleApp
{
public:
    PostProcessingExample()
        : ExampleApp({.title = "Post-Processing Example", .width = 1280, .height = 720,
            .antialias = false}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _platform = std::make_unique<Asset>(
            "statue", AssetType::CONTAINER, assetPath("models/scifi-platform.glb"));
        _mosquito = std::make_unique<Asset>(
            "mosquito", AssetType::CONTAINER, assetPath("models/MosquitoInAmber.glb"));
        _font = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));

        Texture* helipadTexture = _helipad->resourceAs<Texture>();
        ContainerResource* platformContainer = _platform->resourceAs<ContainerResource>();
        ContainerResource* mosquitoContainer = _mosquito->resourceAs<ContainerResource>();
        _fontResource = _font->resourceAs<FontResource>();
        if (!helipadTexture || !platformContainer || !mosquitoContainer) {
            spdlog::error("Failed to load the helipad atlas, scifi-platform.glb or MosquitoInAmber.glb");
            return false;
        }

        // Setup skydome with low intensity
        scene()->setEnvAtlas(helipadTexture);
        scene()->setSkyboxMip(2);
        scene()->setExposure(0.3f);

        // Disable skydome rendering itself, we use the camera clear colour instead
        if (const auto& layers = scene()->layers()) {
            if (const auto skybox = layers->getLayerByName("Skybox")) {
                skybox->setEnabled(false);
            }
        }

        // Platform
        auto* platformEntity = platformContainer->instantiateRenderEntity();
        platformEntity->setLocalScale(10.0f, 10.0f, 10.0f);
        root()->addChild(platformEntity);

        // Emissive materials whose intensity the "emissive" setting drives
        const std::unordered_set<std::string> emissiveNames = {"Light_Upper_Light-Upper_0", "Emissive_Cyan__0"};
        for (auto* render : platformEntity->findComponents<RenderComponent>()) {
            if (!render->entity() || !emissiveNames.contains(render->entity()->name())) {
                continue;
            }
            for (auto* meshInstance : render->meshInstances()) {
                if (auto* material = meshInstance ? dynamic_cast<StandardMaterial*>(meshInstance->material()) : nullptr) {
                    _emissiveMaterials.push_back(material);
                }
            }
        }
        if (_emissiveMaterials.empty()) {
            spdlog::warn("No emissive platform materials found — the emissive setting will do nothing");
        }

        // Mosquito in amber
        _mosquitoEntity = mosquitoContainer->instantiateRenderEntity();
        _mosquitoEntity->setLocalScale(600.0f, 600.0f, 600.0f);
        _mosquitoEntity->setLocalPosition(0.0f, 20.0f, 0.0f);
        root()->addChild(_mosquitoEntity);

        // Three emissive boxes
        _boxes = {
            createBox(100.0f, 20.0f, 0.0f, 1.0f, 0.0f, 0.0f, 60.0f),
            createBox(-50.0f, 20.0f, 100.0f, 0.0f, 1.0f, 0.0f, 60.0f),
            createBox(90.0f, 20.0f, -80.0f, 1.0f, 1.0f, 0.25f, 50.0f),
        };

        // Camera
        auto* cameraEntity = createCamera(Vector3(0.0f, 40.0f, -220.0f));
        _cameraComp = cameraEntity->findComponent<CameraComponent>();
        if (!_cameraComp || !_cameraComp->camera()) {
            spdlog::error("Camera component missing");
            return false;
        }
        _cameraComp->camera()->setFarClip(500.0f);
        _cameraComp->camera()->setFov(80.0f);
        cameraEntity->lookAt(0.0f, 0.0f, 100.0f);

        const Vector3 focusPoint = entityBounds(_mosquitoEntity).center();
        _controls = addOrbitControls(cameraEntity, focusPoint);
        _controls->setZoomRange(Vector2(0.0f, 190.0f));
        _controls->storeResetState();

        // Shadow casting directional light
        auto* light = createDirectionalLight(Vector3(80.0f, 10.0f, 0.0f), _lightColor, 80.0f, true);
        _light = light->findComponent<LightComponent>();
        if (_light) {
            _light->setRange(400.0f);
            _light->setShadowResolution(4096);
            _light->setShadowDistance(400.0f);
            _light->setShadowBias(0.2f);
            _light->setShadowNormalBias(0.05f);
        }

        createUi();

        // CameraFrame: rendering.sceneColorMap = true
        _cameraComp->requestSceneColorMap(true);
        applySettings();

        spdlog::info("Post-processing controls (stand-in for upstream's panel):");
        spdlog::info("  E enabled   M tonemapping   1/2 scale -/+   3/4 background -/+   5/6 emissive -/+");
        spdlog::info("  B bloom   [ ] bloom intensity   ; ' bloom blur level");
        spdlog::info("  G grading   C colour enhance   V vignette   X fringing   T TAA");
        spdlog::info("Orbit: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, R reset, Esc quit");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        auto& s = _settings;
        switch (event.key.key) {
        case SDLK_E: s.enabled = !s.enabled; break;
        case SDLK_M: {
            static constexpr std::array<int, 6> modes = {
                TONEMAP_LINEAR, TONEMAP_FILMIC, TONEMAP_HEJL, TONEMAP_ACES, TONEMAP_ACES2, TONEMAP_NEUTRAL};
            const auto it = std::find(modes.begin(), modes.end(), s.tonemapping);
            const size_t index = it == modes.end() ? 0 : static_cast<size_t>(it - modes.begin()) + 1;
            s.tonemapping = modes[index % modes.size()];
            break;
        }
        case SDLK_1: s.scale = std::max(0.2f, s.scale - 0.2f); break;
        case SDLK_2: s.scale = std::min(2.0f, s.scale + 0.2f); break;
        case SDLK_3: s.background = std::max(0.0f, s.background - 2.0f); break;
        case SDLK_4: s.background = std::min(50.0f, s.background + 2.0f); break;
        case SDLK_5: s.emissive = std::max(0.0f, s.emissive - 25.0f); break;
        case SDLK_6: s.emissive = std::min(400.0f, s.emissive + 25.0f); break;
        case SDLK_B: s.bloomEnabled = !s.bloomEnabled; break;
        case SDLK_LEFTBRACKET: s.bloomIntensity = std::max(0.0f, s.bloomIntensity - 5.0f); break;
        case SDLK_RIGHTBRACKET: s.bloomIntensity = std::min(100.0f, s.bloomIntensity + 5.0f); break;
        case SDLK_SEMICOLON: s.bloomBlurLevel = std::max(1, s.bloomBlurLevel - 1); break;
        case SDLK_APOSTROPHE: s.bloomBlurLevel = std::min(16, s.bloomBlurLevel + 1); break;
        case SDLK_G: s.gradingEnabled = !s.gradingEnabled; break;
        case SDLK_C: s.colorEnhanceEnabled = !s.colorEnhanceEnabled; break;
        case SDLK_V: s.vignetteEnabled = !s.vignetteEnabled; break;
        case SDLK_X: s.fringingEnabled = !s.fringingEnabled; break;
        case SDLK_T: s.taaEnabled = !s.taaEnabled; break;
        default: return false;
        }
        applySettings();
        return true;
    }

    void update(const float dt) override
    {
        _angle += dt;

        // Scale the boxes
        for (size_t i = 0; i < _boxes.size(); ++i) {
            const float offset = kPi * 2.0f * static_cast<float>(i) / static_cast<float>(_boxes.size());
            const float scale = 25.0f + std::sin(_angle + offset) * 10.0f;
            _boxes[i]->setLocalScale(scale, scale, scale);
        }

        // Rotate the mosquito
        _mosquitoEntity->setLocalEulerAngles(0.0f, _angle * 30.0f, 0.0f);

    }

    void destroy() override
    {
        _emissiveMaterials.clear();
    }

private:
    // Helper to create an emissive box primitive
    Entity* createBox(const float x, const float y, const float z,
        const float r, const float g, const float b, const float emissive)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        material->setEmissive(Color(r, g, b, 1.0f));
        material->setEmissiveIntensity(emissive);
        _materials.push_back(material);
        return createPrimitive("box", material.get(), Vector3(x, y, z));
    }

    void createUi()
    {
        FontResource* font = _fontResource;
        if (!font) {
            spdlog::warn("label font failed to load — labels will be missing");
        }

        // A 2D screen to place UI on
        _screen = createScreen();
        Entity* screenEntity = _screen->entity();

        const auto addLabel = [&](const std::string& name, const std::string& text, const float x,
                                  const float y, const int layer) {
            auto* label = new Entity();
            label->setName(name);
            label->setEngine(engine());
            auto* element = static_cast<ElementComponent*>(label->addComponent<ElementComponent>());
            if (element) {
                element->setup({.type = ElementType::Text,
                    .anchor = Vector4(x, y, 0.5f, 0.5f),
                    .pivot = Vector2(0.5f, 0.1f)});
                element->setText(text);
                // Very bright colour to affect the bloom (not correct, as sRGB is valid
                // only in 0..1, but UI exposes no emissive intensity)
                element->setColor(Color(18.0f, 15.0f, 5.0f, 1.0f));
                element->setFontSize(28);
                // Alignment zero: the line at the box's bottom left.
                element->setHorizontalAlign(ElementHorizontalAlign::Left);
                element->setVerticalAlign(0.0f);
                element->setWrapLines(false);
                element->setLayers({layer});
                if (font) {
                    element->setFontResource(font);
                }
            }
            screenEntity->addChild(label);
        };

        // Add a label on the world layer, which will be affected by post-processing
        addLabel("WorldUI", "Text on the World layer affected by post-processing", 0.1f, 0.9f, LAYERID_WORLD);

        // Add a label on the UI layer, which will be rendered after the post-processing
        addLabel("TopUI", "Text on the UI layer after the post-processing", 0.1f, 0.1f, LAYERID_UI);
    }

    void applySettings()
    {
        const auto& s = _settings;

        // Background
        _cameraComp->camera()->setClearColor(Color(
            _lightColor.r * s.background, _lightColor.g * s.background, _lightColor.b * s.background, 1.0f));
        if (_light) {
            _light->setIntensity(s.background);
        }

        // Emissive
        for (auto* material : _emissiveMaterials) {
            material->setEmissiveIntensity(s.emissive);
        }

        auto rendering = _cameraComp->rendering();
        auto taa = _cameraComp->taa();

        // Scene
        rendering.renderTargetScale = s.enabled ? s.scale : 1.0f;
        rendering.toneMapping = s.tonemapping;
        _cameraComp->setToneMapping(s.tonemapping);

        // TAA
        taa.enabled = s.enabled && s.taaEnabled;
        taa.jitter = s.taaJitter;

        // Bloom: lerp(0, 0.1, intensity / 100)
        rendering.bloomIntensity = (s.enabled && s.bloomEnabled) ? 0.1f * (s.bloomIntensity / 100.0f) : 0.0f;
        rendering.bloomBlurLevel = s.bloomBlurLevel;

        // Grading
        rendering.gradingEnabled = s.enabled && s.gradingEnabled;
        rendering.gradingSaturation = s.gradingSaturation;
        rendering.gradingBrightness = s.gradingBrightness;
        rendering.gradingContrast = s.gradingContrast;

        // Colour enhance
        const bool enhance = s.enabled && s.colorEnhanceEnabled;
        rendering.colorEnhanceShadows = enhance ? s.colorEnhanceShadows : 0.0f;
        rendering.colorEnhanceHighlights = enhance ? s.colorEnhanceHighlights : 0.0f;
        rendering.colorEnhanceMidtones = enhance ? s.colorEnhanceMidtones : 0.0f;
        rendering.colorEnhanceVibrance = enhance ? s.colorEnhanceVibrance : 0.0f;
        rendering.colorEnhanceDehaze = enhance ? s.colorEnhanceDehaze : 0.0f;

        // Vignette: intensity 0 when disabled
        rendering.vignetteEnabled = s.enabled && s.vignetteEnabled;
        rendering.vignetteInner = s.vignetteInner;
        rendering.vignetteOuter = s.vignetteOuter;
        rendering.vignetteCurvature = s.vignetteCurvature;
        rendering.vignetteIntensity = rendering.vignetteEnabled ? s.vignetteIntensity : 0.0f;
        rendering.vignetteColor[0] = rendering.vignetteColor[1] = rendering.vignetteColor[2] = 0.0f;

        // Fringing
        rendering.fringingIntensity = (s.enabled && s.fringingEnabled) ? s.fringingIntensity : 0.0f;

        _cameraComp->setRendering(rendering);
        _cameraComp->setTaa(taa);

        spdlog::info(
            "[settings] enabled={} scale={:.1f} background={:.0f} emissive={:.0f} tonemap={} "
            "bloom={}({:.0f}, blur {}) grading={} enhance={} vignette={} fringing={} taa={}",
            s.enabled, s.scale, s.background, s.emissive, tonemapName(s.tonemapping),
            s.bloomEnabled, s.bloomIntensity, s.bloomBlurLevel, s.gradingEnabled,
            s.colorEnhanceEnabled, s.vignetteEnabled, s.fringingEnabled, s.taaEnabled);
    }

    Settings _settings;
    const Color _lightColor{1.0f, 0.7f, 0.1f, 1.0f};

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _platform;
    std::unique_ptr<Asset> _mosquito;
    std::unique_ptr<Asset> _font;
    FontResource* _fontResource = nullptr;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<StandardMaterial*> _emissiveMaterials;
    std::vector<Entity*> _boxes;

    Entity* _mosquitoEntity = nullptr;
    CameraComponent* _cameraComp = nullptr;
    LightComponent* _light = nullptr;
    ScreenComponent* _screen = nullptr;
    CameraControls* _controls = nullptr;
    float _angle = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(PostProcessingExample)
