// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/common-widgets.
//
// A game's settings, built from the common widgets: a slider made from a scrollbar, a toggle
// and a radio group made from buttons, a progress bar whose fill follows its anchor, and a
// modal dialog that asks before the settings are reset.
//
// DEVIATION: upstream cancels the touchend of the canvas, so a browser's emulated mouse events
// after a tap cannot click whatever is behind a dialog button that closed; the element input
// here drops the mouse events SDL synthesizes from touches, so there is nothing to cancel.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiAtlas.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/components/scrollbar/scrollbarComponent.h"
#include "framework/components/scrollbar/scrollbarComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/sprite.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

namespace
{
    const Color ORANGE(1.0f, 0.55f, 0.2f, 1.0f);
    const Color PANEL(0.16f, 0.18f, 0.23f, 1.0f);
    const Color DARK(0.1f, 0.11f, 0.14f, 1.0f);
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);
    const Color SLATE(0.26f, 0.29f, 0.36f, 1.0f);

    const Vector4 CENTER_ANCHOR(0.5f, 0.5f, 0.5f, 0.5f);
    const Vector4 LEFT_ANCHOR(0.0f, 0.5f, 0.0f, 0.5f);
    const Vector2 LEFT_PIVOT(0.0f, 0.5f);
    const Vector4 RIGHT_ANCHOR(1.0f, 0.5f, 1.0f, 0.5f);
    const Vector2 RIGHT_PIVOT(1.0f, 0.5f);
    const Vector4 STRETCH_ANCHOR(0.0f, 0.0f, 1.0f, 1.0f);

    FontResource* fontOf(Asset& asset)
    {
        const auto res = asset.resource();
        return res && std::holds_alternative<FontResource*>(*res) ? std::get<FontResource*>(*res) : nullptr;
    }

    Texture* textureOf(Asset& asset)
    {
        const auto res = asset.resource();
        return res && std::holds_alternative<Texture*>(*res) ? std::get<Texture*>(*res) : nullptr;
    }

    struct ElementProps
    {
        ElementType type = ElementType::Image;
        std::shared_ptr<Sprite> sprite;
        int spriteFrame = 0;
        Color color = LIGHT;
        float opacity = 1.0f;
        Vector4 anchor = CENTER_ANCHOR;
        Vector2 pivot = Vector2(0.5f, 0.5f);
        std::optional<Vector4> margin;
        std::optional<float> width;
        std::optional<float> height;
        bool useInput = false;
        FontResource* font = nullptr;
        std::string text;
        std::optional<int> fontSize;
    };
}

class CommonWidgetsExample final: public ExampleApp
{
public:
    CommonWidgetsExample()
        : ExampleApp({.title = "Common Widgets", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ScreenComponentSystem>();
        options.registerComponentSystem<ElementComponentSystem>();
        options.registerComponentSystem<ButtonComponentSystem>();
        options.registerComponentSystem<ScrollbarComponentSystem>();
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
        Texture* atlasTexture = textureOf(*_uiAtlasTexture);
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

        auto atlas = createUiAtlas(atlasTexture);
        _panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        _track = std::make_shared<Sprite>(atlas, std::vector<std::string>{"track"}, 2.0f, SpriteRenderMode::Sliced);
        _parts = std::make_shared<Sprite>(atlas,
            std::vector<std::string>{"knob", "checkbox", "check", "radio", "radio-dot"});

        // The settings panel, and its rows. Each row has its label anchored to its left edge and
        // its widget to its right edge, so both follow the row's width, which follows the panel's
        _settings = createElement(screenEntity, {.sprite = _panel, .color = PANEL, .width = 640.0f, .height = 580.0f});
        createElement(_settings->entity(), {.type = ElementType::Text, .font = _bold, .text = "Settings", .fontSize = 40})
            ->entity()->setLocalPosition(0.0f, 236.0f, 0.0f);

        // A slider: a scrollbar whose handle is smaller than its track. Its value goes from 0 to 1
        Entity* volumeRow = row("Volume", 150.0f);
        _volume = createElement(volumeRow, {.sprite = _track, .color = DARK, .anchor = RIGHT_ANCHOR,
                                            .pivot = RIGHT_PIVOT, .height = 20.0f});
        _volume->entity()->setLocalPosition(-80.0f, 0.0f, 0.0f);
        ElementComponent* handle = createElement(_volume->entity(), {.sprite = _parts, .spriteFrame = 0,
            .anchor = LEFT_ANCHOR, .pivot = LEFT_PIVOT, .height = 36.0f, .useInput = true});
        _slider = static_cast<ScrollbarComponent*>(_volume->entity()->addComponent<ScrollbarComponent>());
        _slider->setOrientation(Orientation::Horizontal);
        _slider->setHandleEntity(handle->entity());
        ElementComponent* percent = createElement(volumeRow, {.type = ElementType::Text, .color = MUTED,
            .anchor = RIGHT_ANCHOR, .pivot = RIGHT_PIVOT, .fontSize = 26});
        _slider->on("set:value", [percent](const float value) {
            percent->setText(std::to_string(static_cast<int>(std::round(value * 100.0f))) + "%");
        });

        // A toggle: a button whose check shows while music is on
        _musicToggle = createCheck(row("Music", 70.0f), 1, RIGHT_ANCHOR, RIGHT_PIVOT);
        _musicToggle->on("click", [this]() {
            _musicOn = !_musicOn;
            _musicToggle->entity()->findByName("check")->setEnabled(_musicOn);
        });

        // A radio group: a group element of such buttons, of which only one is on at a time. It is
        // under its label, so that it fits narrow panels too
        ElementComponent* difficulty = createElement(row("Difficulty", 10.0f), {.type = ElementType::Group,
            .anchor = LEFT_ANCHOR, .pivot = LEFT_PIVOT, .width = 0.0f});
        difficulty->entity()->setLocalPosition(0.0f, -56.0f, 0.0f);
        const std::array<const char*, 3> names{{"Easy", "Normal", "Hard"}};
        for (size_t i = 0; i < names.size(); ++i) {
            ButtonComponent* option = createCheck(difficulty->entity(), 3, CENTER_ANCHOR, Vector2(0.5f, 0.5f));
            option->entity()->setLocalPosition(20.0f + static_cast<float>(i) * 150.0f, 0.0f, 0.0f);
            createElement(option->entity(), {.type = ElementType::Text, .anchor = LEFT_ANCHOR, .pivot = LEFT_PIVOT,
                                             .text = names[i], .fontSize = 24})
                ->entity()->setLocalPosition(48.0f, 0.0f, 0.0f);
            _options.push_back(option);
        }
        for (ButtonComponent* option : _options) {
            option->on("click", [this, option]() {
                for (ButtonComponent* other : _options) {
                    other->entity()->findByName("check")->setEnabled(other == option);
                }
            });
        }

        // A progress bar: the fill's right anchor follows the value, so it takes that share of the
        // track, and the track keeps a border inside the bar
        _bar = createElement(row("Update 1.2", -126.0f), {.sprite = _track, .color = DARK, .anchor = RIGHT_ANCHOR,
                                                          .pivot = RIGHT_PIVOT, .height = 30.0f});
        ElementComponent* fillTrack = createElement(_bar->entity(), {.type = ElementType::Group,
            .anchor = STRETCH_ANCHOR, .margin = Vector4(4.0f, 4.0f, 4.0f, 4.0f)});
        _fill = createElement(fillTrack->entity(), {.sprite = _track, .color = ORANGE, .anchor = STRETCH_ANCHOR,
                                                    .margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f)});

        // The dialog covers the rest of the screen, and is last in the hierarchy, so it is drawn on
        // top and receives input first. Its backdrop catches every press that misses the panel
        _dialog = createElement(screenEntity, {.type = ElementType::Group, .anchor = STRETCH_ANCHOR,
                                               .margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f)})->entity();
        ElementComponent* backdrop = createElement(_dialog, {.color = Color(0.0f, 0.0f, 0.0f, 1.0f), .opacity = 0.6f,
            .anchor = STRETCH_ANCHOR, .margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f), .useInput = true});
        ElementComponent* box = createElement(_dialog, {.sprite = _panel, .color = PANEL, .width = 440.0f,
                                                        .height = 240.0f, .useInput = true});
        createElement(box->entity(), {.type = ElementType::Text, .font = _bold, .text = "Reset all settings?",
                                      .fontSize = 32})
            ->entity()->setLocalPosition(0.0f, 50.0f, 0.0f);

        ButtonComponent* reset = createButton(_settings->entity(), "Reset", SLATE, 0.0f, -220.0f);
        ButtonComponent* cancel = createButton(box->entity(), "Cancel", SLATE, -100.0f, -50.0f);
        ButtonComponent* confirm = createButton(box->entity(), "Reset all", ORANGE, 100.0f, -50.0f);
        reset->on("click", [this]() { _dialog->setEnabled(true); });
        cancel->on("click", [this]() { _dialog->setEnabled(false); });
        backdrop->on("click", [this]() { _dialog->setEnabled(false); });
        confirm->on("click", [this]() {
            applyDefaults();
            _dialog->setEnabled(false);
        });
        _dialog->setEnabled(false);

        layout();
        applyDefaults();
        return true;
    }

    // An update downloads over six seconds, and starts again two seconds later
    void update(const float dt) override
    {
        layout();
        _time = std::fmod(_time + dt, 8.0f);
        _fill->setAnchor(Vector4(0.0f, 0.0f, std::min(_time / 6.0f, 1.0f), 1.0f));
    }

private:
    ElementComponent* createElement(Entity* parent, const ElementProps& props) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        ElementDesc desc{.type = props.type, .anchor = props.anchor, .pivot = props.pivot, .margin = props.margin};
        desc.width = props.width;
        desc.height = props.height;
        desc.useInput = props.useInput;
        element->setup(desc);
        if (props.sprite) {
            element->setSprite(props.sprite);
            element->setSpriteFrame(props.spriteFrame);
        }
        element->setColor(props.color);
        element->setOpacity(props.opacity);
        if (props.type == ElementType::Text) {
            element->setFontResource(props.font ? props.font : _font);
            if (props.fontSize) {
                element->setFontSize(*props.fontSize);
            }
            element->setText(props.text);
        }
        parent->addChild(entity);
        return element;
    }

    Entity* row(const std::string& name, const float y)
    {
        ElementComponent* entity = createElement(_settings->entity(), {.type = ElementType::Group, .height = 60.0f});
        entity->entity()->setLocalPosition(0.0f, y, 0.0f);
        createElement(entity->entity(), {.type = ElementType::Text, .anchor = LEFT_ANCHOR, .pivot = LEFT_PIVOT,
                                         .text = name, .fontSize = 28});
        _rows.push_back(entity);
        return entity->entity();
    }

    // A button whose look shows its state: a box or a ring, and a mark in it while it is on. The
    // mark is the frame after the box's
    ButtonComponent* createCheck(Entity* parent, const int frame, const Vector4& anchor, const Vector2& pivot)
    {
        ElementComponent* element = createElement(parent, {.sprite = _parts, .spriteFrame = frame, .color = MUTED,
            .anchor = anchor, .pivot = pivot, .width = 40.0f, .height = 40.0f, .useInput = true});
        auto* button = static_cast<ButtonComponent*>(element->entity()->addComponent<ButtonComponent>());
        button->setImageEntity(element->entity());
        button->setHoverTint(LIGHT);
        button->setPressedTint(MUTED);
        createElement(element->entity(), {.sprite = _parts, .spriteFrame = frame + 1, .color = ORANGE, .width = 40.0f,
                                          .height = 40.0f})
            ->entity()->setName("check");
        return button;
    }

    // A button with a label
    ButtonComponent* createButton(Entity* parent, const std::string& text, const Color& color, const float x,
                                  const float y)
    {
        ElementComponent* element = createElement(parent, {.sprite = _panel, .color = color, .width = 180.0f,
                                                            .height = 64.0f, .useInput = true});
        element->entity()->setLocalPosition(x, y, 0.0f);
        auto* button = static_cast<ButtonComponent*>(element->entity()->addComponent<ButtonComponent>());
        button->setImageEntity(element->entity());
        button->setHoverTint(Color(color.r * 0.8f + 0.2f, color.g * 0.8f + 0.2f, color.b * 0.8f + 0.2f, 1.0f));
        button->setPressedTint(Color(color.r * 0.7f, color.g * 0.7f, color.b * 0.7f, 1.0f));
        const bool orange = color.r == ORANGE.r && color.g == ORANGE.g && color.b == ORANGE.b;
        createElement(element->entity(), {.type = ElementType::Text, .color = orange ? DARK : LIGHT, .font = _bold,
                                          .text = text, .fontSize = 26});
        return button;
    }

    // Put everything back as it was at the start: volume 80%, music on, and Normal difficulty
    void applyDefaults()
    {
        _slider->setValue(0.8f);
        _musicOn = true;
        _musicToggle->entity()->findByName("check")->setEnabled(true);
        for (size_t i = 0; i < _options.size(); ++i) {
            _options[i]->entity()->findByName("check")->setEnabled(i == 1);
        }
    }

    // A narrower panel on portrait canvases, which the rows follow. The slider's handle is a
    // fraction of its track, so it is set for each length to keep the knob round
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
        _settings->setWidth(portrait ? 490.0f : 640.0f);
        for (ElementComponent* row : _rows) {
            row->setWidth(_settings->width() - 80.0f);
        }
        _volume->setWidth(portrait ? 180.0f : 280.0f);
        _slider->setHandleSize(36.0f / _volume->width());
        _bar->setWidth(portrait ? 220.0f : 300.0f);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    FontResource* _bold = nullptr;
    std::shared_ptr<Sprite> _panel;
    std::shared_ptr<Sprite> _track;
    std::shared_ptr<Sprite> _parts;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _settings = nullptr;
    std::vector<ElementComponent*> _rows;
    ElementComponent* _volume = nullptr;
    ScrollbarComponent* _slider = nullptr;
    ButtonComponent* _musicToggle = nullptr;
    bool _musicOn = true;
    std::vector<ButtonComponent*> _options;
    ElementComponent* _bar = nullptr;
    ElementComponent* _fill = nullptr;
    Entity* _dialog = nullptr;
    float _time = 3.0f;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(CommonWidgetsExample)
