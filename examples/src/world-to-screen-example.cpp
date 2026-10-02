// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Port of upstream user-interface/world-to-screen.
//
// Four capsule "fighters" walk circles of their own radius and speed over a dark floor
// under a shadow-casting directional light, seen from a 45-degree camera at (0, 7, 12)
// looking at the origin. Each fighter carries a screen-space tag that follows a point
// 1.4 above it: its name in Roboto Bold with a dark outline, over a 9-sliced health bar
// (a dark track and a coloured fill whose right anchor is the fraction of health left).
// A tag fades with its fighter's distance and hides when the fighter is behind the camera
// or off the canvas. Clicking a tag takes a quarter of that fighter's health, colouring
// the bar from green toward red, and refills it when it runs low. On a portrait window
// the camera keeps the arena's width in view and the tags use a portrait reference
// resolution.
//
// DEVIATIONS:
// - worldToScreen answers in window points (the canvas here), so the tag's screen units are
//   points over the screen's scale, where upstream converts CSS pixels by the pixel ratio.
// - the name is a text element as wide as its tag, centred in it; upstream's is
//   auto-sized to the text, which centres it the same way.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "core/math/matrix4.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/materials/standardMaterial.h"
#include "scene/sprite.h"
#include "scene/textureAtlas.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

namespace
{
    const Color DARK(0.05f, 0.05f, 0.07f, 1.0f);
    const Color GREEN(0.35f, 0.85f, 0.35f, 1.0f);
    const Color RED(0.95f, 0.3f, 0.25f, 1.0f);

    struct FighterDef
    {
        const char* name;
        Color color;
        float radius;
        float speed;
    };

    struct Fighter
    {
        Entity* body = nullptr;
        Entity* tag = nullptr;
        ElementComponent* health = nullptr;
        std::array<ElementComponent*, 3> parts{};
        std::shared_ptr<StandardMaterial> material;
        float hp = 1.0f;
        float radius = 1.0f;
        float speed = 0.0f;
        float angle = 0.0f;
    };
}

class WorldToScreenExample final: public ExampleApp
{
public:
    WorldToScreenExample()
        : ExampleApp({.title = "World To Screen", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

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
        _font = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        FontResource* font = nullptr;
        if (const auto res = _font->resource(); res && std::holds_alternative<FontResource*>(*res)) {
            font = std::get<FontResource*>(*res);
        }
        Texture* atlasTexture = nullptr;
        if (const auto res = _uiAtlasTexture->resource(); res && std::holds_alternative<Texture*>(*res)) {
            atlasTexture = std::get<Texture*>(*res);
        }
        if (!font || !atlasTexture) {
            spdlog::error("Failed to load fonts/roboto-bold.json or ui/ui-atlas.png");
            return false;
        }

        // The track frame of the UI kit atlas.
        _atlas = std::make_shared<TextureAtlas>();
        _atlas->setTexture(atlasTexture);
        _atlas->setFrame("track", {.rect = Vector4(292.0f, 308.0f, 64.0f, 32.0f), .pivot = Vector2(0.5f, 0.5f),
                                   .border = Vector4(16.0f, 16.0f, 16.0f, 16.0f)});
        _track = std::make_shared<Sprite>(_atlas, std::vector<std::string>{"track"}, 4.0f, SpriteRenderMode::Sliced);

        // The arena: a floor, lit from above, and a camera looking down at it
        scene()->setAmbientLight(0.3f, 0.32f, 0.38f);
        _floorMaterial = createMaterial(Color(0.24f, 0.26f, 0.32f, 1.0f));
        createPrimitive("plane", _floorMaterial.get(), Vector3(0.0f, 0.0f, 0.0f), Vector3(200.0f, 1.0f, 200.0f));

        auto* light = createDirectionalLight(Vector3(50.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowType(SHADOW_PCF3_32F);
            lightComp->setShadowDistance(30.0f);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
        }

        auto* cameraEntity = createCamera(Vector3(0.0f, 7.0f, 12.0f));
        cameraEntity->lookAt(0.0f, 0.0f, 0.0f);
        _camera = cameraEntity->findComponent<CameraComponent>();
        _camera->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));
        _camera->camera()->setFov(45.0f);

        // The screen the tags are on
        auto* screenEntity = new Entity();
        screenEntity->setEngine(engine());
        _screen = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
        _screen->setScreenSpace(true);
        _screen->setReferenceResolution(Vector2(1280.0f, 720.0f));
        _screen->setScaleMode(ScreenScaleMode::Blend);
        _screen->setScaleBlend(0.5f);
        root()->addChild(screenEntity);

        // The fighters walk around the arena, each on a circle of its own
        const std::array<FighterDef, 4> defs{{
            {"Aria", Color(1.0f, 0.55f, 0.2f, 1.0f), 2.5f, 0.5f},
            {"Brom", Color(0.3f, 0.6f, 1.0f, 1.0f), 4.5f, -0.3f},
            {"Cai", Color(0.45f, 0.8f, 0.4f, 1.0f), 6.5f, 0.2f},
            {"Dara", Color(0.7f, 0.45f, 0.95f, 1.0f), 4.0f, 0.4f},
        }};
        _fighters.reserve(defs.size());
        for (size_t i = 0; i < defs.size(); ++i) {
            createFighter(screenEntity, font, defs[i], static_cast<float>(i) * 1.7f);
        }

        layout();
        return true;
    }

    // One update walks the fighters and places their tags.
    void update(const float dt) override
    {
        layout();
        const Vector3 overHead(0.0f, 1.4f, 0.0f);
        for (auto& fighter : _fighters) {
            fighter.angle += dt * fighter.speed;
            fighter.body->setPosition(fighter.radius * std::sin(fighter.angle), 1.0f,
                                      fighter.radius * std::cos(fighter.angle));
            const Vector3 head = fighter.body->position() + overHead;
            const Vector3 onCanvas = _camera->worldToScreen(head);

            // Hide the tag when its fighter is behind the camera, which the depth in view
            // space tells, or off the canvas
            const Vector3 view = _camera->entity()->worldTransform().inverse().transformPoint(head);
            const auto [canvasWidth, canvasHeight] = engine()->canvasSize();
            const bool visible = view.getZ() < 0.0f &&
                onCanvas.getX() > 0.0f && onCanvas.getX() < static_cast<float>(canvasWidth) &&
                onCanvas.getY() > 0.0f && onCanvas.getY() < static_cast<float>(canvasHeight);
            fighter.tag->setEnabled(visible);
            if (visible) {
                const float units = 1.0f / std::max(_screen->scale(), 1e-6f);
                fighter.tag->setLocalPosition(onCanvas.getX() * units,
                    (static_cast<float>(canvasHeight) - onCanvas.getY()) * units, 0.0f);
                // Fade the tags of distant fighters, so that the nearest ones stand out
                const float opacity = std::clamp(2.2f - -view.getZ() / 10.0f, 0.35f, 1.0f);
                for (auto* part : fighter.parts) {
                    part->setOpacity(opacity);
                }
            }
        }
    }

private:
    std::shared_ptr<StandardMaterial> createMaterial(const Color& color) const
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setGloss(0.4f);
        return material;
    }

    /// An element on `parent`, centred on it unless `desc` says otherwise.
    ElementComponent* createElement(Entity* parent, const std::string& name, ElementDesc desc) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        entity->setName(name);
        if (!desc.anchor) {
            desc.anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f);
        }
        if (!desc.pivot) {
            desc.pivot = Vector2(0.5f, 0.5f);
        }
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        element->setup(desc);
        parent->addChild(entity);
        return element;
    }

    void createFighter(Entity* screen, FontResource* font, const FighterDef& def, const float angle)
    {
        auto& fighter = _fighters.emplace_back();
        fighter.radius = def.radius;
        fighter.speed = def.speed;
        fighter.angle = angle;
        fighter.material = createMaterial(def.color);
        fighter.body = createPrimitive("capsule", fighter.material.get(), Vector3(0.0f, 1.0f, 0.0f),
                                       Vector3(0.8f, 1.0f, 0.8f));
        fighter.body->setName(def.name);

        // The tag is anchored to the bottom-left corner of the screen, and its pivot is the
        // middle of its bottom edge, so it sits over the point it is placed at. Tapping it
        // hits the fighter
        ElementComponent* tag = createElement(screen, std::string(def.name) + " tag",
            {.type = ElementType::Group, .anchor = Vector4(0.0f, 0.0f, 0.0f, 0.0f), .pivot = Vector2(0.5f, 0.0f),
             .width = 140.0f, .height = 56.0f, .useInput = true});
        fighter.tag = tag->entity();

        ElementComponent* label = createElement(fighter.tag, "name",
            {.type = ElementType::Text, .width = 140.0f, .height = 24.0f});
        label->setFontResource(font);
        label->setText(def.name);
        label->setFontSize(24);
        label->setHorizontalAlign(ElementHorizontalAlign::Center);
        label->setVerticalAlign(0.5f);
        label->setOutlineColor(DARK);
        label->setOutlineThickness(0.5f);
        label->entity()->setLocalPosition(0.0f, 10.0f, 0.0f);

        // The health bar: a dark track, and a fill whose right anchor is the fraction of
        // health left
        ElementComponent* bar = createElement(fighter.tag, "bar",
            {.type = ElementType::Image, .width = 120.0f, .height = 12.0f});
        bar->setSprite(_track);
        bar->setColor(DARK);
        bar->entity()->setLocalPosition(0.0f, -14.0f, 0.0f);

        fighter.health = createElement(bar->entity(), "health",
            {.type = ElementType::Image, .anchor = Vector4(0.0f, 0.0f, 1.0f, 1.0f),
             .margin = Vector4(2.0f, 2.0f, 2.0f, 2.0f)});
        fighter.health->setSprite(_track);
        fighter.health->setColor(GREEN);
        fighter.parts = {label, bar, fighter.health};

        const size_t index = _fighters.size() - 1;
        tag->on("click", [this, index]() {
            Fighter& f = _fighters[index];
            f.hp = f.hp > 0.3f ? f.hp - 0.25f : 1.0f;
            f.health->setAnchor(Vector4(0.0f, 0.0f, f.hp, 1.0f));
            Color color;
            color.lerp(RED, GREEN, f.hp);
            f.health->setColor(color);
        });
    }

    // On portrait canvases, fit the arena to the width of the view rather than its height,
    // and use a portrait reference resolution for the tags
    void layout()
    {
        const auto [w, h] = engine()->canvasSize();
        const bool portrait = h > w;
        if (portrait == _portrait && _laidOut) {
            return;
        }
        _portrait = portrait;
        _laidOut = true;
        _camera->camera()->setHorizontalFov(portrait);
        _screen->setReferenceResolution(portrait ? Vector2(540.0f, 960.0f) : Vector2(1280.0f, 720.0f));
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _font;
    std::unique_ptr<Asset> _uiAtlasTexture;
    std::shared_ptr<TextureAtlas> _atlas;
    std::shared_ptr<Sprite> _track;
    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::vector<Fighter> _fighters;
    CameraComponent* _camera = nullptr;
    ScreenComponent* _screen = nullptr;
    bool _portrait = false;
    bool _laidOut = false;
};

VISUTWIN_EXAMPLE_MAIN(WorldToScreenExample)
