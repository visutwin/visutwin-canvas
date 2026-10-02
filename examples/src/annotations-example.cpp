// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream misc/annotations.
//
// A Mitsubishi F-2 fighter jet over a shadow catcher, under the Shanghai Riverside HDRI shown
// as a sky dome, carries seven numbered hotspots. A hotspot faces the camera at a constant
// size on screen, shows through the jet faintly where the jet hides it, turns orange under
// the mouse, and a click on it opens its tooltip; a click anywhere else closes it. The panel
// on the right is upstream's control panel: hotspot size, the two colours, and the opacity in
// front of and behind geometry.
//
// Model: "Mitsubishi F-2 - Fighter Jet - Free" by bohmerang, Sketchfab, CC BY 4.0.
//
// DEVIATIONS:
// - the control panel is built from engine UI elements (sliders from scrollbars, as in
//   common-widgets), where upstream's is a PCUI panel; each colour picker is a swatch with an
//   R, G and B slider.
// - CameraControls has no `sceneSize`, which scales upstream's fly and pan speeds.
//
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiAtlas.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/components/scrollbar/scrollbarComponent.h"
#include "framework/components/scrollbar/scrollbarComponentSystem.h"
#include "framework/handlers/containerResource.h"
#include "framework/input/elementInput.h"
#include "framework/script/annotation.h"
#include "framework/script/annotationManager.h"
#include "framework/script/shadowCatcher.h"
#include "scene/graphics/envLighting.h"
#include "scene/sprite.h"

using namespace visutwin::canvas;

namespace
{
    const Color PANEL(0.21f, 0.25f, 0.27f, 1.0f);
    const Color TRACK(0.13f, 0.16f, 0.17f, 1.0f);
    const Color KNOB(0.85f, 0.87f, 0.89f, 1.0f);
    const Color LABEL(0.7f, 0.74f, 0.76f, 1.0f);

    constexpr float PANEL_WIDTH = 300.0f;
    constexpr float ROW_HEIGHT = 26.0f;
    constexpr float LABEL_WIDTH = 110.0f;
    constexpr float VALUE_WIDTH = 36.0f;

    struct AnnotationDef
    {
        Vector3 position;
        const char* label;
        const char* title;
        const char* text;
    };

    template <typename T>
    T* resourceOf(Asset& asset)
    {
        const auto res = asset.resource();
        return res && std::holds_alternative<T*>(*res) ? std::get<T*>(*res) : nullptr;
    }
}

class AnnotationsExample final: public ExampleApp
{
public:
    AnnotationsExample(): ExampleApp({.title = "Annotations"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ScreenComponentSystem>();
        options.registerComponentSystem<ElementComponentSystem>();
        options.registerComponentSystem<ScrollbarComponentSystem>();
        _elementInput = std::make_shared<ElementInput>();
        options.elementInput = _elementInput;
    }

    bool create() override
    {
        _jetAsset = std::make_unique<Asset>("jet-fighter", AssetType::CONTAINER, assetPath("models/jet-fighter.glb"));
        _hdrAsset = std::make_unique<Asset>("shanghai", AssetType::TEXTURE, assetPath("hdri/shanghai-riverside-4k.hdr"));
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _boldAsset = std::make_unique<Asset>("bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        auto* jet = resourceOf<ContainerResource>(*_jetAsset);
        auto* hdr = resourceOf<Texture>(*_hdrAsset);
        _font = resourceOf<FontResource>(*_fontAsset);
        _bold = resourceOf<FontResource>(*_boldAsset);
        auto* atlasTexture = resourceOf<Texture>(*_uiAtlasTexture);
        if (!jet || !hdr || !_font || !_bold || !atlasTexture) {
            spdlog::error("Failed to load the jet fighter, the HDRI, the Roboto fonts or the UI atlas");
            return false;
        }

        // Setup HDR environment
        _skybox.reset(EnvLighting::generateSkyboxCubemap(device().get(), hdr));
        _envAtlas.reset(EnvLighting::generateAtlas(device().get(), hdr));
        scene()->setSkybox(_skybox.get());
        scene()->setEnvAtlas(_envAtlas.get());

        // Setup sky dome
        scene()->setSkyType(SKYTYPE_DOME);
        scene()->sky()->node()->setLocalScale(Vector3(50.0f, 50.0f, 50.0f));
        scene()->sky()->node()->setLocalPosition(Vector3(0.0f, 0.0f, 0.0f));
        scene()->sky()->setCenter(Vector3(0.0f, 0.1f, 0.0f));

        // Create camera entity
        Entity* camera = createCamera(Vector3(12.0f, 8.3f, 4.5f));
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        cameraComponent->camera()->setClearColor(Color(0.5f, 0.6f, 0.9f, 1.0f));
        cameraComponent->camera()->setFarClip(500.0f);
        cameraComponent->setToneMapping(TONEMAP_ACES2);

        // Add camera controls
        CameraControls* controls = addOrbitControls(camera, Vector3(-1.0f, 1.5f, 0.0f));
        controls->setPitchRange(Vector2(-90.0f, 0.0f));
        controls->setZoomRange(Vector2(5.0f, 25.0f));

        // Create directional light
        Entity* light = createDirectionalLight(Vector3(0.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        auto* lightComponent = light->findComponent<LightComponent>();
        lightComponent->setShadowDistance(30.0f);
        lightComponent->setShadowIntensity(0.6f);
        lightComponent->setShadowResolution(1024);
        lightComponent->setShadowType(SHADOW_VSM_16F);

        // Create a wrapper entity for the jet fighter
        auto* jetFighter = new Entity();
        jetFighter->setEngine(engine());
        jetFighter->setName("jet-fighter");
        jetFighter->setPosition(-2.0f, 1.6f, 0.0f);
        jetFighter->setLocalEulerAngles(0.0f, 0.0f, 3.0f);   // under the root, local is world
        root()->addChild(jetFighter);

        // Instantiate the model as a child of the wrapper
        Entity* jetModel = jet->instantiateRenderEntity();
        jetModel->setEngine(engine());
        for (auto* render : jetModel->findComponents<RenderComponent>()) {
            render->setCastShadows(true);
        }
        jetFighter->addChild(jetModel);

        // Add annotation manager to the jet fighter entity
        jetFighter->addComponent<ScriptComponent>();
        _manager = jetFighter->script()->create<AnnotationManager>();
        _manager->setFonts(_font, _bold);

        // Add annotations to the jet fighter
        const std::array<AnnotationDef, 7> annotations{{
            {Vector3(5.5f, 1.2f, 0.0f), "1", "Cockpit Canopy",
             "Transparent canopy offering visibility and housing the pilot's controls."},
            {Vector3(8.0f, 0.25f, 0.0f), "2", "Nose Cone & Radar",
             "Houses the advanced radar system for targeting and navigation."},
            {Vector3(5.0f, -0.5f, 0.0f), "3", "Inlet Ducts",
             "Provides airflow to the engines, crucial for maintaining thrust."},
            {Vector3(0.5f, 0.0f, 5.1f), "4", "Wingtip Missile Rails",
             "Can be equipped with AIM-9 Sidewinder missiles for air-to-air combat."},
            {Vector3(-4.0f, 0.0f, 0.0f), "5", "Jet Engine Nozzles",
             "Dual afterburning turbofan engines for high-speed performance."},
            {Vector3(1.0f, -1.0f, -1.0f), "6", "Main Landing Gear",
             "Retractable gear for safe takeoff and landing on runways."},
            {Vector3(2.0f, 0.0f, -3.1f), "7", "Forward Leading-Edge Flaps",
             "Enhance maneuverability during high-speed or low-speed flight."},
        }};
        for (const AnnotationDef& def : annotations) {
            jetFighter->addChild(createAnnotation(def));
        }

        // Create shadow catcher
        auto* shadowCatcher = new Entity();
        shadowCatcher->setEngine(engine());
        shadowCatcher->setName("shadowCatcher");
        shadowCatcher->addComponent<ScriptComponent>();
        shadowCatcher->script()->create<ShadowCatcher>()->setPlaneScale(15.0f);
        root()->addChild(shadowCatcher);

        createControls(atlasTexture);
        return true;
    }

    void destroy() override
    {
        // The scene borrows both; it is torn down after this
        if (scene()) {
            scene()->setSkybox(nullptr);
            scene()->setEnvAtlas(nullptr);
        }
    }

private:
    // Create an annotation entity
    Entity* createAnnotation(const AnnotationDef& def) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        entity->setName(std::string("annotation") + def.label);
        entity->setLocalPosition(def.position);
        entity->addComponent<ScriptComponent>();
        // The script initializes, and registers with the manager, once the entity is added
        // to the scene, by which time its properties are set
        auto* annotation = entity->script()->create<Annotation>();
        annotation->setLabel(def.label);
        annotation->setTitle(def.title);
        annotation->setText(def.text);
        return entity;
    }

    // ── The control panel ────────────────────────────────────────────────────

    ElementComponent* createElement(Entity* parent, const ElementDesc& desc) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        element->setup(desc);
        parent->addChild(entity);
        return element;
    }

    ElementComponent* createText(Entity* parent, const std::string& text, const Vector4& anchor,
                                 const Vector2& pivot, FontResource* font, const int size) const
    {
        ElementComponent* element = createElement(parent, {.type = ElementType::Text, .anchor = anchor, .pivot = pivot});
        element->setFontResource(font);
        element->setFontSize(size);
        element->setColor(LABEL);
        element->setText(text);
        return element;
    }

    /// A row of the panel: its label on the left, and the group its widget goes in on the right.
    Entity* row(const std::string& label)
    {
        // Anchored to one corner: setting the position of an element whose anchors are
        // split re-derives its margins
        ElementComponent* group = createElement(_panel->entity(),
            {.type = ElementType::Group, .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f), .pivot = Vector2(0.0f, 1.0f),
             .width = PANEL_WIDTH - 24.0f, .height = ROW_HEIGHT});
        group->entity()->setLocalPosition(12.0f, -_nextRowY, 0.0f);
        _nextRowY += ROW_HEIGHT;
        createText(group->entity(), label, Vector4(0.0f, 0.5f, 0.0f, 0.5f), Vector2(0.0f, 0.5f), _font, 13);
        return group->entity();
    }

    /// A slider from min to max, its value printed beside it with `precision` decimals.
    ScrollbarComponent* slider(const std::string& label, const float min, const float max, const int precision,
                               std::function<void(float)> onChange)
    {
        Entity* parent = row(label);
        constexpr float width = PANEL_WIDTH - 24.0f - LABEL_WIDTH - VALUE_WIDTH - 8.0f;
        ElementComponent* track = createElement(parent,
            {.type = ElementType::Image, .anchor = Vector4(0.0f, 0.5f, 0.0f, 0.5f), .pivot = Vector2(0.0f, 0.5f),
             .width = width, .height = 8.0f, .useInput = true});
        track->setSprite(_track);
        track->setColor(TRACK);
        track->entity()->setLocalPosition(LABEL_WIDTH, 0.0f, 0.0f);
        ElementComponent* handle = createElement(track->entity(),
            {.type = ElementType::Image, .anchor = Vector4(0.0f, 0.5f, 0.0f, 0.5f), .pivot = Vector2(0.0f, 0.5f),
             .height = 16.0f, .useInput = true});
        handle->setSprite(_knob);
        handle->setColor(KNOB);
        auto* bar = static_cast<ScrollbarComponent*>(track->entity()->addComponent<ScrollbarComponent>());
        bar->setOrientation(Orientation::Horizontal);
        bar->setHandleEntity(handle->entity());
        bar->setHandleSize(16.0f / width);

        ElementComponent* value = createText(parent, "", Vector4(1.0f, 0.5f, 1.0f, 0.5f), Vector2(1.0f, 0.5f),
                                             _font, 13);
        const auto show = [=](const float t) {
            const float v = min + (max - min) * t;
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
            value->setText(buffer);
            return v;
        };
        // A value equal to the current one fires nothing, so the text starts at it
        show(bar->value());
        bar->on("set:value", [=](const float t) { onChange(show(t)); });
        return bar;
    }

    /// Upstream's ColorPicker: a swatch of the colour, then a slider per channel.
    void colorPicker(const std::string& label, const Color& initial, std::function<void(const Color&)> onChange)
    {
        Entity* parent = row(label);
        ElementComponent* swatch = createElement(parent,
            {.type = ElementType::Image, .anchor = Vector4(0.0f, 0.5f, 0.0f, 0.5f), .pivot = Vector2(0.0f, 0.5f),
             .width = 48.0f, .height = 16.0f});
        swatch->entity()->setLocalPosition(LABEL_WIDTH, 0.0f, 0.0f);
        swatch->setColor(initial);

        auto color = std::make_shared<Color>(initial);
        const std::array<const char*, 3> channels{{"  R", "  G", "  B"}};
        for (int c = 0; c < 3; ++c) {
            ScrollbarComponent* bar = slider(channels[c], 0.0f, 1.0f, 2, [=](const float v) {
                (c == 0 ? color->r : c == 1 ? color->g : color->b) = v;
                swatch->setColor(*color);
                onChange(*color);
            });
            bar->setValue(c == 0 ? initial.r : c == 1 ? initial.g : initial.b);
        }
    }

    void createControls(Texture* atlasTexture)
    {
        auto atlas = createUiAtlas(atlasTexture);
        _panelSprite = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 8.0f,
                                                SpriteRenderMode::Sliced);
        _track = std::make_shared<Sprite>(atlas, std::vector<std::string>{"track"}, 4.0f, SpriteRenderMode::Sliced);
        _knob = std::make_shared<Sprite>(atlas, std::vector<std::string>{"knob"});

        auto* screenEntity = new Entity();
        screenEntity->setEngine(engine());
        screenEntity->setName("controls");
        auto* screen = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
        screen->setScreenSpace(true);
        root()->addChild(screenEntity);

        // Panel at the top right; a press on it is the panel's, not the camera's
        constexpr int rows = 11;
        _panel = createElement(screenEntity,
            {.type = ElementType::Image, .anchor = Vector4(1.0f, 1.0f, 1.0f, 1.0f), .pivot = Vector2(1.0f, 1.0f),
             .width = PANEL_WIDTH, .height = 44.0f + rows * ROW_HEIGHT, .useInput = true});
        _panel->setSprite(_panelSprite);
        _panel->setColor(PANEL);
        _panel->entity()->setLocalPosition(-10.0f, -10.0f, 0.0f);
        _panel->on("mousedown", [](ElementInputEvent* event) { event->stopPropagation(); });
        _panel->on("touchstart", [](ElementInputEvent* event) { event->stopPropagation(); });

        ElementComponent* header = createText(_panel->entity(), "Annotations", Vector4(0.0f, 1.0f, 0.0f, 1.0f),
                                              Vector2(0.0f, 1.0f), _bold, 15);
        header->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        header->entity()->setLocalPosition(12.0f, -10.0f, 0.0f);
        _nextRowY = 36.0f;

        // Set default values for controls
        slider("Hotspot Size", 10.0f, 50.0f, 0, [this](const float v) { _manager->setHotspotSize(v); })
            ->setValue((25.0f - 10.0f) / 40.0f);
        colorPicker("Hotspot Color", Color(0.8f, 0.8f, 0.8f, 1.0f),
                    [this](const Color& c) { _manager->setHotspotColor(c); });
        colorPicker("Hover Color", Color(1.0f, 0.4f, 0.0f, 1.0f),
                    [this](const Color& c) { _manager->setHoverColor(c); });
        slider("Opacity", 0.0f, 1.0f, 2, [this](const float v) { _manager->setOpacity(v); })->setValue(1.0f);
        slider("Behind Opacity", 0.0f, 1.0f, 2, [this](const float v) { _manager->setBehindOpacity(v); })
            ->setValue(0.25f);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _jetAsset;
    std::unique_ptr<Asset> _hdrAsset;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    std::unique_ptr<Texture> _skybox;
    std::unique_ptr<Texture> _envAtlas;
    FontResource* _font = nullptr;
    FontResource* _bold = nullptr;
    AnnotationManager* _manager = nullptr;

    std::shared_ptr<Sprite> _panelSprite;
    std::shared_ptr<Sprite> _track;
    std::shared_ptr<Sprite> _knob;
    ElementComponent* _panel = nullptr;
    float _nextRowY = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(AnnotationsExample)
