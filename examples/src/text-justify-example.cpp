// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/text-justify.
//
// A page of a codex with reader settings. Justify stretches every wrapped line to both edges
// of the text element by widening the gaps between its words, and alignment places the lines
// that aren't stretched, including the last one. The buttons above the page switch them; an
// outline marks each chosen setting.
//
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
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
    const Color ORANGE(1.0f, 0.55f, 0.2f, 1.0f);
    const Color INK(0.22f, 0.18f, 0.14f, 1.0f);
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);

    FontResource* fontOf(Asset& asset)
    {
        const auto res = asset.resource();
        return res && std::holds_alternative<FontResource*>(*res) ? std::get<FontResource*>(*res) : nullptr;
    }

    struct Alignment
    {
        ElementComponent* button;
        ElementHorizontalAlign align;
    };
}

class TextJustifyExample final: public ExampleApp
{
public:
    TextJustifyExample()
        : ExampleApp({.title = "Text Justify", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ScreenComponentSystem>();
        options.registerComponentSystem<ElementComponentSystem>();
        options.registerComponentSystem<ButtonComponentSystem>();
        _elementInput = std::make_shared<ElementInput>();
        options.elementInput = _elementInput;
    }

    bool create() override
    {
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _boldAsset = std::make_unique<Asset>("bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        _font = fontOf(*_fontAsset);
        _bold = fontOf(*_boldAsset);
        Texture* atlasTexture = nullptr;
        if (const auto res = _uiAtlasTexture->resource(); res && std::holds_alternative<Texture*>(*res)) {
            atlasTexture = std::get<Texture*>(*res);
        }
        if (!_font || !_bold || !atlasTexture) {
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
        _screenEntity = screenEntity;

        // Frames of upstream's UI kit atlas (ui-atlas.mjs), 2 pixels per unit
        auto atlas = std::make_shared<TextureAtlas>();
        atlas->setTexture(atlasTexture);
        const Vector4 border32(32.0f, 32.0f, 32.0f, 32.0f);
        atlas->setFrame("panel", {.rect = Vector4(548.0f, 660.0f, 128.0f, 128.0f), .pivot = Vector2(0.5f, 0.5f),
                                  .border = border32});
        atlas->setFrame("panel-outline", {.rect = Vector4(684.0f, 660.0f, 128.0f, 128.0f),
                                          .pivot = Vector2(0.5f, 0.5f), .border = border32});
        _panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        _outline = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel-outline"}, 2.0f,
                                            SpriteRenderMode::Sliced);

        // The page, and its chapter heading
        _page = createElement(screenEntity, ElementType::Image);
        _page->setSprite(_panel);
        _page->setColor(Color(0.96f, 0.92f, 0.84f, 1.0f));
        ElementComponent* heading = createElement(_page->entity(), ElementType::Text, Vector4(0.5f, 1.0f, 0.5f, 1.0f),
                                                  Vector2(0.5f, 1.0f));
        setText(heading, _bold, 34, INK, "The Fall of Eldermere");
        heading->entity()->setLocalPosition(0.0f, -34.0f, 0.0f);

        // The chapter. Wrapping needs a width to wrap at, so auto width is off, and the text is
        // kept at the top of the element, so that it doesn't move as the number of lines changes
        _chapter = createElement(_page->entity(), ElementType::Text, Vector4(0.5f, 1.0f, 0.5f, 1.0f),
                                 Vector2(0.5f, 1.0f));
        // Upstream applies every property before laying the text out
        _chapter->setAutoWidth(false);
        _chapter->setAutoHeight(false);
        _chapter->setWrapLines(true);
        _chapter->setJustify(true);
        _chapter->setHorizontalAlign(ElementHorizontalAlign::Left);
        _chapter->setVerticalAlign(1.0f);
        _chapter->setLineHeight(34.0f);
        setText(_chapter, _font, 24, INK,
                "For three hundred years the towers of Eldermere watched over the valley, and no army "
                "reached its walls. Then came the winter the river froze, when the mountain clans "
                "crossed the ice by night. By morning the gates had fallen, and the great library burned "
                "for nine days. Of its scholars, only one escaped, carrying the last of the star charts.");
        _chapter->entity()->setLocalPosition(0.0f, -96.0f, 0.0f);

        _justifyButton = createButton("Justify");
        _alignments = {{createButton("Left"), ElementHorizontalAlign::Left},
                       {createButton("Center"), ElementHorizontalAlign::Center},
                       {createButton("Right"), ElementHorizontalAlign::Right}};
        _bar = {_justifyButton, _alignments[0].button, _alignments[1].button, _alignments[2].button};

        _justifyButton->entity()->findComponent<ButtonComponent>()->on("click", [this] {
            _justify = !_justify;
            apply();
        });
        for (const Alignment& alignment : _alignments) {
            const ElementHorizontalAlign align = alignment.align;
            alignment.button->entity()->findComponent<ButtonComponent>()->on("click", [this, align] {
                _align = align;
                apply();
            });
        }
        apply();
        layout();
        return true;
    }

    void update(float /*dt*/) override
    {
        layout();
    }

private:
    ElementComponent* createElement(Entity* parent, const ElementType type,
                                    const Vector4& anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
                                    const Vector2& pivot = Vector2(0.5f, 0.5f)) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        element->setup({.type = type, .anchor = anchor, .pivot = pivot});
        parent->addChild(entity);
        return element;
    }

    static void setText(ElementComponent* element, FontResource* font, const int fontSize, const Color& color,
                        const std::string& text)
    {
        element->setFontResource(font);
        element->setFontSize(fontSize);
        element->setColor(color);
        element->setText(text);
    }

    /// A button of the settings bar, whose outline shows when its setting is chosen.
    ElementComponent* createButton(const std::string& text)
    {
        ElementComponent* image = createElement(_screenEntity, ElementType::Image);
        image->setHeight(60.0f);
        image->setUseInput(true);
        image->setSprite(_panel);
        image->setColor(Color(0.2f, 0.23f, 0.29f, 1.0f));
        auto* button = static_cast<ButtonComponent*>(image->entity()->addComponent<ButtonComponent>());
        button->setImageEntity(image->entity());
        button->setHoverTint(Color(0.27f, 0.31f, 0.39f, 1.0f));
        button->setPressedTint(Color(0.15f, 0.17f, 0.22f, 1.0f));
        auto* chosenEntity = new Entity();
        chosenEntity->setEngine(engine());
        chosenEntity->setName("chosen");
        auto* chosen = static_cast<ElementComponent*>(chosenEntity->addComponent<ElementComponent>());
        chosen->setup({.type = ElementType::Image, .anchor = Vector4(0.0f, 0.0f, 1.0f, 1.0f),
                       .pivot = Vector2(0.5f, 0.5f), .margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f)});
        chosen->setSprite(_outline);
        chosen->setColor(ORANGE);
        image->entity()->addChild(chosenEntity);
        ElementComponent* label = createElement(image->entity(), ElementType::Text);
        label->entity()->setName("label");
        setText(label, _bold, 24, LIGHT, text);
        return image;
    }

    // Apply the settings to the chapter, and show which are chosen
    void apply()
    {
        _chapter->setJustify(_justify);
        _chapter->setHorizontalAlign(_align);
        const auto show = [](ElementComponent* button, const bool chosen) {
            if (auto* outline = dynamic_cast<Entity*>(button->entity()->findByName("chosen"))) {
                outline->setEnabled(chosen);
            }
            if (auto* label = dynamic_cast<Entity*>(button->entity()->findByName("label"))) {
                label->findComponent<ElementComponent>()->setColor(chosen ? ORANGE : LIGHT);
            }
        };
        show(_justifyButton, _justify);
        for (const Alignment& alignment : _alignments) {
            show(alignment.button, alignment.align == _align);
        }
    }

    // A wide page on landscape canvases, and a narrow one on portrait canvases, where the lines
    // hold fewer words and the gaps that justifying widens are larger
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
        _page->setWidth(portrait ? 500.0f : 680.0f);
        _page->setHeight(portrait ? 430.0f : 380.0f);
        _page->entity()->setLocalPosition(0.0f, portrait ? -40.0f : -30.0f, 0.0f);
        _chapter->setWidth(portrait ? 440.0f : 600.0f);
        _chapter->setHeight(portrait ? 320.0f : 260.0f);
        for (size_t i = 0; i < _bar.size(); ++i) {
            _bar[i]->setWidth(portrait ? 112.0f : 150.0f);
            _bar[i]->entity()->setLocalPosition((static_cast<float>(i) - 1.5f) * (portrait ? 120.0f : 170.0f),
                                                portrait ? 330.0f : 260.0f, 0.0f);
        }
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    FontResource* _bold = nullptr;
    std::shared_ptr<Sprite> _panel;
    std::shared_ptr<Sprite> _outline;
    Entity* _screenEntity = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _page = nullptr;
    ElementComponent* _chapter = nullptr;
    ElementComponent* _justifyButton = nullptr;
    std::vector<Alignment> _alignments;
    std::vector<ElementComponent*> _bar;
    bool _justify = true;
    ElementHorizontalAlign _align = ElementHorizontalAlign::Left;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(TextJustifyExample)
