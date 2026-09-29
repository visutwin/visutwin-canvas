// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/text-markup.
//
// An "Adventure Log" on a sliced panel from upstream's UI atlas: a Roboto Bold heading, then
// lines of Roboto Regular with markup turned on — coloured names and items, an outlined
// damage number, a critical hit with a shadow around coloured text, and an escaped bracket
// (`\[AFK]`). One line never closes its colour tag, which is a markup error: it is drawn as
// written, tags included, under a note saying so. The lines wrap at the log's width and are
// stacked by their measured heights; the log is wide on a landscape window and narrow on a
// portrait one.
//
// DEVIATIONS:
// - glyphs of different markup styles are separate draws (upstream draws a page in one mesh
//   with the style in vertex attributes); overlapping neighbours' outlines can order
//   differently.
//
#include <memory>
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
    const Color LIGHT(0.9f, 0.92f, 0.95f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);

    FontResource* fontOf(Asset& asset)
    {
        const auto res = asset.resource();
        return res && std::holds_alternative<FontResource*>(*res) ? std::get<FontResource*>(*res) : nullptr;
    }
}

class UiTextMarkupExample final: public ExampleApp
{
public:
    UiTextMarkupExample()
        : ExampleApp({.title = "UI Text Markup", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

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
        _font = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _bold = std::make_unique<Asset>("bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        FontResource* font = fontOf(*_font);
        FontResource* bold = fontOf(*_bold);
        Texture* atlasTexture = nullptr;
        if (const auto res = _uiAtlasTexture->resource(); res && std::holds_alternative<Texture*>(*res)) {
            atlasTexture = std::get<Texture*>(*res);
        }
        if (!font || !bold || !atlasTexture) {
            spdlog::error("Failed to load the Roboto fonts or ui/ui-atlas.png");
            return false;
        }

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));

        auto* screenEntity = new Entity();
        screenEntity->setEngine(engine());
        _screen = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
        _screen->setScreenSpace(true);
        _screen->setReferenceResolution(Vector2(1280.0f, 720.0f));
        _screen->setScaleMode(ScreenScaleMode::Blend);
        _screen->setScaleBlend(0.5f);
        root()->addChild(screenEntity);

        // The panel frame of upstream's UI kit atlas (ui-atlas.mjs), 2 pixels per unit.
        auto atlas = std::make_shared<TextureAtlas>();
        atlas->setTexture(atlasTexture);
        atlas->setFrame("panel", {.rect = Vector4(548.0f, 660.0f, 128.0f, 128.0f), .pivot = Vector2(0.5f, 0.5f),
                                  .border = Vector4(32.0f, 32.0f, 32.0f, 32.0f)});
        auto* logEntity = new Entity();
        logEntity->setEngine(engine());
        _log = static_cast<ElementComponent*>(logEntity->addComponent<ElementComponent>());
        _log->setup({.type = ElementType::Image, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
                     .pivot = Vector2(0.5f, 0.5f)});
        _log->setSprite(std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f,
                                                 SpriteRenderMode::Sliced));
        _log->setColor(Color(0.14f, 0.16f, 0.2f, 1.0f));
        screenEntity->addChild(logEntity);

        addLine(bold, "Adventure Log", 30, Color(1.0f, 0.55f, 0.2f, 1.0f), false);
        // Each line turns markup on. A tag styles the text up to its closing tag, and tags
        // can be nested
        for (const char* text : {
                 R"([color="#9aa3b5"]You enter the Sunken Crypt.[/color])",
                 R"(You found the [color="#ffcc00"]golden key[/color]!)",
                 R"([color="#7fc8ff"]Aria[/color] hits the [color="#ff7a6e"]Cave Troll[/color] for )"
                 R"([outline color="#c43c2c" thickness="0.8"]42[/outline] damage.)",
                 R"([shadow color="#8a3000" offset="0.6"][color="#ffb347"]Critical hit![/color][/shadow] )"
                 R"(The [color="#ff7a6e"]Cave Troll[/color] is defeated.)",
                 R"([color="#7fc8ff"]Brom[/color]: back in five, I am \[AFK])"}) {
            addLine(font, text, 26, LIGHT, true);
        }
        // A tag that is never closed is a markup error: the engine logs a warning and draws
        // the whole line as written, tags included
        addLine(font, R"([color="#88e088"]Quest complete: Wolves at the Gate)", 26, LIGHT, true);
        addLine(font, "The line above never closes its color tag, so it is drawn as written.", 20, MUTED, false);

        layout();
        return true;
    }

    void update(float /*dt*/) override
    {
        layout();
    }

private:
    /// A line of the log. Lines wrap at the width of the log, and are stacked by `layout`.
    void addLine(FontResource* font, const std::string& text, const int fontSize, const Color& color,
                 const bool markup)
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* line = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        line->setup({.type = ElementType::Text, .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f),
                     .pivot = Vector2(0.0f, 1.0f)});
        // Upstream applies every property before laying the text out.
        line->setAutoWidth(false);
        line->setWrapLines(true);
        line->setHorizontalAlign(ElementHorizontalAlign::Left);
        line->setVerticalAlign(1.0f);
        line->setLineHeight(34.0f);
        line->setFontResource(font);
        line->setFontSize(fontSize);
        line->setColor(color);
        line->setEnableMarkup(markup);
        line->setText(text);
        _log->entity()->addChild(entity);
        _lines.push_back(line);
    }

    // A wide log on landscape canvases and a narrow one on portrait canvases. The lines wrap
    // at its width, and are stacked one below the other by their heights
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
        const float width = portrait ? 500.0f : 820.0f;
        float y = 26.0f;
        for (auto* line : _lines) {
            line->setWidth(width - 64.0f);
            line->entity()->setLocalPosition(32.0f, -y, 0.0f);
            y += line->height() + 12.0f;
        }
        _log->setWidth(width);
        _log->setHeight(y + 14.0f);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _font;
    std::unique_ptr<Asset> _bold;
    std::unique_ptr<Asset> _uiAtlasTexture;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _log = nullptr;
    std::vector<ElementComponent*> _lines;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(UiTextMarkupExample)
