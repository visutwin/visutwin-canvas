// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
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
// Initial settings are upstream's `data.set('data', ...)` defaults, value for
// value: render scale 1.8, background 6, emissive 200, ACES, bloom on at
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
//  - Screen-space text is always drawn by a separate orthographic overlay camera
//    on the UI layer; a Screen child cannot be placed on the World layer. So the
//    "World layer" label is ALSO drawn after post-processing and does not bloom.
//    The screen has no scaleMode/scaleBlend, and element anchors are not laid out
//    by the element system, so the labels are positioned from upstream's anchors
//    by hand every frame, with the left edge on the anchor as upstream's
//    thumbnail shows.
//  - Upstream's orbit camera script is CameraControls in orbit mode, aimed at the
//    mosquito's bounds centre (what upstream's script does on initialize), with
//    the zoom range capped at upstream's distanceMax of 190.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "../cameraControls.h"
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

    // Upstream's panel values (data.set('data', ...)).
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
        : ExampleApp({.title = "Post-Processing Example", .width = 1280, .height = 720}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ScreenComponentSystem>();
        options.registerComponentSystem<ElementComponentSystem>();
        _elementInput = std::make_shared<ElementInput>();
        options.elementInput = _elementInput;
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
        // DEVIATION: upstream loads its arial atlas; this is Liberation Sans, which is
        // metric-compatible with Arial (identical advances) and OFL-licensed.
        _font = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/liberation-sans.json"));

        const auto helipadResource = _helipad->resource();
        const auto platformResource = _platform->resource();
        const auto mosquitoResource = _mosquito->resource();
        if (const auto res = _font->resource(); res && std::holds_alternative<FontResource*>(*res)) {
            _fontResource = std::get<FontResource*>(*res);
        }
        if (!helipadResource || !platformResource || !mosquitoResource) {
            spdlog::error("Failed to load the helipad atlas, scifi-platform.glb or MosquitoInAmber.glb");
            return false;
        }

        // Setup skydome with low intensity
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));
        scene()->setSkyboxMip(2);
        scene()->setExposure(0.3f);

        // Disable skydome rendering itself, we use the camera clear colour instead
        if (const auto& layers = scene()->layers()) {
            if (const auto skybox = layers->getLayerByName("Skybox")) {
                skybox->setEnabled(false);
            }
        }

        // Platform
        auto* platformEntity = std::get<ContainerResource*>(*platformResource)->instantiateRenderEntity();
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
        _mosquitoEntity = std::get<ContainerResource*>(*mosquitoResource)->instantiateRenderEntity();
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
        _cameraComp->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX, LAYERID_IMMEDIATE});
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

        layoutUi();
    }

    void preRender() override
    {
        _elementInput->syncTextElements();
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

        // Orthographic overlay camera for the UI layer, after the main camera.
        auto* uiCameraEntity = createCamera(Vector3(0.0f, 0.0f, 10.0f));
        _uiCamera = uiCameraEntity->findComponent<CameraComponent>();
        if (_uiCamera && _uiCamera->camera()) {
            _uiCamera->camera()->setProjection(ProjectionType::Orthographic);
            _uiCamera->camera()->setOrthoHeight(static_cast<float>(windowHeight()) * 0.5f);
            _uiCamera->camera()->setClearColorBuffer(false);
            _uiCamera->camera()->setClearDepthBuffer(true);
            _uiCamera->camera()->setClearStencilBuffer(true);
            _uiCamera->setLayers({LAYERID_UI});
            _uiCamera->setPriority(1);
        }

        // A 2D screen to place UI on
        auto* screenEntity = new Entity();
        screenEntity->setEngine(engine());
        _screen = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
        if (_screen) {
            _screen->setReferenceResolution(Vector2(1280.0f, 720.0f));
            _screen->setScreenSpace(true);
        }
        root()->addChild(screenEntity);

        const auto addLabel = [&](const std::string& text, const float x, const float y) {
            auto* label = new Entity();
            label->setName(text);
            label->setEngine(engine());
            auto* element = static_cast<ElementComponent*>(label->addComponent<ElementComponent>());
            if (element) {
                element->setType(ElementType::Text);
                element->setText(text);
                // Very bright colour to affect the bloom (upstream's comment: not correct,
                // as sRGB is valid only in 0..1, but UI exposes no emissive intensity)
                element->setColor(Color(18.0f, 15.0f, 5.0f, 1.0f));
                element->setAnchor(Vector4(x, y, 0.5f, 0.5f));
                element->setFontSize(28);
                element->setHeight(28.0f);
                element->setWidth(900.0f);
                element->setPivot(Vector2(0.5f, 0.1f));
                element->setHorizontalAlign(ElementHorizontalAlign::Left);
                element->setWrapLines(false);
                if (font) {
                    element->setFontResource(font);
                }
            }
            screenEntity->addChild(label);
            _labels.push_back({element, x, y});
        };

        addLabel("Text on the World layer affected by post-processing", 0.1f, 0.9f);
        addLabel("Text on theUI layer after the post-processing", 0.1f, 0.1f);
    }

    // Upstream anchors are fractions with y UP; the element system positions UI text
    // in pixels from the TOP-left, so the anchor is converted here.
    void layoutUi()
    {
        int windowW = 1;
        int windowH = 1;
        SDL_GetWindowSize(window(), &windowW, &windowH);
        float uiWidth = static_cast<float>(windowW);
        float uiHeight = static_cast<float>(windowH);
        if (_screen) {
            _screen->updateScaleFromWindow(windowW, windowH);
            const float scale = std::max(_screen->scale(), 1e-6f);
            uiWidth = _screen->resolution().x / scale;
            uiHeight = _screen->resolution().y / scale;
        }
        if (_uiCamera && _uiCamera->camera()) {
            _uiCamera->camera()->setOrthoHeight(uiHeight * 0.5f);
        }
        // The glyph layout starts a left-aligned line at -pivot.x * width, so the
        // entity is offset by that much to put the line's left edge on the anchor,
        // which is where upstream's thumbnail shows it.
        for (const auto& label : _labels) {
            if (label.element && label.element->entity()) {
                const float x = label.anchorX * uiWidth + label.element->pivot().x * label.element->width();
                label.element->entity()->setLocalPosition(x, (1.0f - label.anchorY) * uiHeight, 0.0f);
            }
        }
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

        // Bloom: upstream lerp(0, 0.1, intensity / 100)
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

        // Vignette: upstream sets intensity 0 when disabled
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

    struct Label
    {
        ElementComponent* element = nullptr;
        float anchorX = 0.0f;
        float anchorY = 0.0f;
    };

    Settings _settings;
    const Color _lightColor{1.0f, 0.7f, 0.1f, 1.0f};

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _platform;
    std::unique_ptr<Asset> _mosquito;
    std::unique_ptr<Asset> _font;
    FontResource* _fontResource = nullptr;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<StandardMaterial*> _emissiveMaterials;
    std::vector<Entity*> _boxes;
    std::vector<Label> _labels;

    Entity* _mosquitoEntity = nullptr;
    CameraComponent* _cameraComp = nullptr;
    CameraComponent* _uiCamera = nullptr;
    LightComponent* _light = nullptr;
    ScreenComponent* _screen = nullptr;
    CameraControls* _controls = nullptr;
    float _angle = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(PostProcessingExample)
