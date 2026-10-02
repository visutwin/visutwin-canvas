// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.07.2026
//
// Port of upstream user-interface/text.
//
// A game-over screen: a "Game Over" title in Roboto Bold with an outline and a soft shadow,
// a letter-spaced subtitle, a sliced panel holding two columns of stats — labels aligned
// left, values in bold aligned right, both at the same size and line height — a tip that
// wraps at its element's width and is centred line by line, and a countdown whose text is
// set only when its number changes while its opacity pulses every frame. On a portrait
// window the screen takes a portrait reference resolution and a narrower tip, and scales to
// whichever axis has less room so the whole screen stays in view.
//
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/sprite.h"
#include "scene/textureAtlas.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

namespace
{
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);

    /// The text properties set per element. Unset fields keep the
    /// element's defaults (auto size, centred, the font size as the line height).
    struct TextProps
    {
        std::string text;
        FontResource* font = nullptr;
        int fontSize = 32;
        Color color = LIGHT;
        std::optional<float> lineHeight;
        std::optional<float> spacing;
        std::optional<float> width;
        std::optional<float> height;
        bool autoWidth = true;
        bool autoHeight = true;
        bool wrapLines = false;
        ElementHorizontalAlign alignX = ElementHorizontalAlign::Center;
        float alignY = 0.5f;
    };
}

class UiTextExample final: public ExampleApp
{
public:
    UiTextExample()
        : ExampleApp({.title = "UI Text", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _boldAsset = std::make_unique<Asset>("bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        _font = _fontAsset->resourceAs<FontResource>();
        FontResource* bold = _boldAsset->resourceAs<FontResource>();
        Texture* atlasTexture = _uiAtlasTexture->resourceAs<Texture>();
        if (!_font || !bold || !atlasTexture) {
            spdlog::error("Failed to load the Roboto fonts or ui/ui-atlas.png");
            return false;
        }

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));

        _screen = createScreen();
        Entity* screenEntity = _screen->entity();

        // The title, with an outline and a soft shadow drawn by the font's shader
        ElementComponent* title = createText(screenEntity, {.text = "Game Over", .font = bold, .fontSize = 64,
                                                            .color = Color(1.0f, 0.55f, 0.2f, 1.0f)});
        title->setOutlineColor(Color(0.45f, 0.14f, 0.0f, 1.0f));
        title->setOutlineThickness(0.4f);
        title->setShadowColor(Color(0.0f, 0.0f, 0.0f, 0.8f));
        title->setShadowOffset(Vector2(0.14f, -0.22f));
        title->entity()->setLocalPosition(0.0f, 240.0f, 0.0f);

        // Spacing widens the gaps between the letters
        createText(screenEntity, {.text = "YOUR QUEST ENDS HERE", .fontSize = 22, .color = MUTED, .spacing = 1.4f})
            ->entity()->setLocalPosition(0.0f, 182.0f, 0.0f);

        // The stats: two text elements of the same size and line height, one aligned to the
        // left and one to the right, over a sliced panel (the atlas's `panel`, 2 px per unit)
        auto atlas = std::make_shared<TextureAtlas>();
        atlas->setTexture(atlasTexture);
        atlas->setFrame("panel", {.rect = Vector4(548.0f, 660.0f, 128.0f, 128.0f), .pivot = Vector2(0.5f, 0.5f),
                                  .border = Vector4(32.0f, 32.0f, 32.0f, 32.0f)});
        auto* panelEntity = new Entity();
        panelEntity->setEngine(engine());
        auto* panel = static_cast<ElementComponent*>(panelEntity->addComponent<ElementComponent>());
        panel->setup({.type = ElementType::Image, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
                      .pivot = Vector2(0.5f, 0.5f), .width = 440.0f, .height = 210.0f});
        panel->setSprite(std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f,
                                                  SpriteRenderMode::Sliced));
        panel->setColor(Color(0.16f, 0.18f, 0.23f, 1.0f));
        panelEntity->setLocalPosition(0.0f, 30.0f, 0.0f);
        screenEntity->addChild(panelEntity);

        const TextProps column{.fontSize = 26, .lineHeight = 44.0f, .width = 360.0f, .height = 176.0f,
                               .autoWidth = false, .autoHeight = false};
        TextProps labels = column;
        labels.text = "Score\nEnemies defeated\nTime survived\nBest score";
        labels.color = MUTED;
        labels.alignX = ElementHorizontalAlign::Left;
        createText(panelEntity, labels);
        TextProps values = column;
        values.text = "12,480\n87\n14:32\n15,200";
        values.font = bold;
        values.alignX = ElementHorizontalAlign::Right;
        createText(panelEntity, values);

        // A tip that wraps at the width of its element, centered line by line
        _tip = createText(screenEntity, {.text = "Tip: shields stop arrows but not fireballs. Step to the side as "
                                                 "soon as a mage raises its staff.",
                                         .fontSize = 24, .color = MUTED, .lineHeight = 32.0f, .width = 560.0f,
                                         .autoWidth = false, .wrapLines = true});
        _tip->entity()->setLocalPosition(0.0f, -140.0f, 0.0f);

        _countdown = createText(screenEntity, {.font = bold, .fontSize = 28});
        _countdown->entity()->setLocalPosition(0.0f, -250.0f, 0.0f);

        layout();
        return true;
    }

    // Count down to the next run. Setting the text lays it out again, so it is set only when
    // the number changes, while the pulse changes its opacity, which doesn't
    void update(const float dt) override
    {
        layout();
        _time += dt;
        const int seconds = 5 - static_cast<int>(std::floor(std::fmod(_time, 5.0f)));
        if (seconds != _shown) {
            _shown = seconds;
            _countdown->setText("Restarting in " + std::to_string(seconds));
        }
        _countdown->setOpacity(0.55f + 0.45f * std::cos(_time * 3.14159265f * 2.0f));
    }

private:
    /// A text element centred on its parent: Roboto Regular in
    /// the light colour unless `props` says otherwise.
    ElementComponent* createText(Entity* parent, const TextProps& props) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        ElementDesc desc{.type = ElementType::Text, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
                         .pivot = Vector2(0.5f, 0.5f)};
        desc.width = props.width;
        desc.height = props.height;
        element->setup(desc);
        // Every property is applied before the text is laid out.
        element->setAutoWidth(props.autoWidth);
        element->setAutoHeight(props.autoHeight);
        element->setWrapLines(props.wrapLines);
        element->setHorizontalAlign(props.alignX);
        element->setVerticalAlign(props.alignY);
        if (props.lineHeight) {
            element->setLineHeight(*props.lineHeight);
        }
        if (props.spacing) {
            element->setSpacing(*props.spacing);
        }
        element->setFontResource(props.font ? props.font : _font);
        element->setFontSize(props.fontSize);
        element->setColor(props.color);
        element->setText(props.text);
        parent->addChild(entity);
        return element;
    }

    // Use a portrait reference resolution on portrait canvases, with a narrower tip, and scale
    // to whichever axis has less room, so the whole screen stays in view
    void layout()
    {
        const auto [w, h] = engine()->canvasSize();
        if (w == _laidOutWidth && h == _laidOutHeight) {
            return;
        }
        _laidOutWidth = w;
        _laidOutHeight = h;
        const bool portrait = h > w;
        const Vector2 reference = portrait ? Vector2(540.0f, 960.0f) : Vector2(1280.0f, 720.0f);
        _screen->setReferenceResolution(reference);
        _screen->setScaleBlend(static_cast<float>(w) / reference.x > static_cast<float>(h) / reference.y ? 1.0f : 0.0f);
        _tip->setWidth(portrait ? 460.0f : 560.0f);
    }

    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _tip = nullptr;
    ElementComponent* _countdown = nullptr;
    float _time = 0.0f;
    int _shown = 0;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(UiTextExample)
