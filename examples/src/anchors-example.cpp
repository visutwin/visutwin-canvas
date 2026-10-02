// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// Port of upstream user-interface/anchors.
//
// An in-game chat window laid out with anchors. Its title bar, message log and input row are
// anchored to its edges, so they follow the window as it changes size, and the score keeps to
// the corner of the screen. Drag the grip in the window's corner to resize it, and send a
// message.
//
#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
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
#include "framework/input/elementInput.h"
#include "scene/sprite.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

namespace
{
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);
    const Color DARK(0.1f, 0.11f, 0.14f, 1.0f);
}

class AnchorsExample final: public ExampleApp
{
public:
    AnchorsExample()
        : ExampleApp({.title = "Anchors", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options, {.button = true});
    }

    bool create() override
    {
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _boldAsset = std::make_unique<Asset>("bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        _landscapeTexture = std::make_unique<Asset>("landscape", AssetType::TEXTURE, assetPath("ui/landscape.png"),
            AssetData{.mipmaps = true});
        _font = _fontAsset->resourceAs<FontResource>();
        FontResource* bold = _boldAsset->resourceAs<FontResource>();
        Texture* atlasTexture = _uiAtlasTexture->resourceAs<Texture>();
        Texture* landscape = _landscapeTexture->resourceAs<Texture>();
        if (!_font || !bold || !atlasTexture || !landscape) {
            spdlog::error("Failed to load the Roboto fonts, ui/ui-atlas.png or ui/landscape.png");
            return false;
        }

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));

        _screen = createScreen();
        Entity* screenEntity = _screen->entity();

        auto atlas = createUiAtlas(atlasTexture);
        auto panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        auto icons = std::make_shared<Sprite>(atlas, std::vector<std::string>{"icon-next", "icon-grip"});
        const Vector4 fill(0.0f, 0.0f, 1.0f, 1.0f);
        const Vector4 zero(0.0f, 0.0f, 0.0f, 0.0f);

        // The game behind the interface: a picture that covers the whole screen, whatever its shape
        createElement(screenEntity, {.texture = landscape, .fitMode = ElementFitMode::Cover, .anchor = fill,
                                     .margin = zero});

        // The score sits in the top-right corner of the screen, 20 units in from each edge
        createElement(screenEntity, {.type = ElementType::Text, .anchor = Vector4(1.0f, 1.0f, 1.0f, 1.0f),
                                     .pivot = Vector2(1.0f, 1.0f), .font = bold, .text = "1250", .fontSize = 48})
            ->entity()->setLocalPosition(-20.0f, -20.0f, 0.0f);

        // The chat window hangs from its top-left corner, so it grows to the right and downwards
        _chat = createElement(screenEntity, {.sprite = panel, .color = Color(0.16f, 0.18f, 0.23f, 1.0f),
            .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f), .pivot = Vector2(0.0f, 1.0f), .width = 460.0f,
            .height = 380.0f});

        // The title bar is split across the top of the window, so its width follows the window's.
        // Its title is anchored to its left end, and the number of players online to its right end
        ElementComponent* bar = createElement(_chat->entity(), {.sprite = panel, .color = DARK,
            .anchor = Vector4(0.0f, 1.0f, 1.0f, 1.0f), .pivot = Vector2(0.5f, 1.0f)});
        bar->setLeft(0.0f);
        bar->setRight(0.0f);
        bar->setHeight(56.0f);
        createElement(bar->entity(), {.type = ElementType::Text, .anchor = Vector4(0.0f, 0.5f, 0.0f, 0.5f),
                                      .pivot = Vector2(0.0f, 0.5f), .font = bold, .text = "Party chat", .fontSize = 26})
            ->entity()->setLocalPosition(20.0f, 0.0f, 0.0f);
        createElement(bar->entity(), {.type = ElementType::Text, .color = MUTED,
                                      .anchor = Vector4(1.0f, 0.5f, 1.0f, 0.5f), .pivot = Vector2(1.0f, 0.5f),
                                      .text = "3 online", .fontSize = 22})
            ->entity()->setLocalPosition(-20.0f, 0.0f, 0.0f);

        // The messages fill the window apart from a 20 unit margin, and leave room for the title
        // bar and the input row. The log wraps to the width of the area, and the area, a mask,
        // crops old lines
        ElementComponent* area = createElement(_chat->entity(), {.sprite = panel, .anchor = fill,
            .margin = Vector4(20.0f, 76.0f, 20.0f, 76.0f), .mask = true});
        createElement(area->entity(), {.sprite = panel, .color = DARK, .anchor = fill, .margin = zero});
        _log = createElement(area->entity(), {.type = ElementType::Text, .anchor = fill,
            .margin = Vector4(14.0f, 12.0f, 14.0f, 12.0f), .fontSize = 22, .lineHeight = 30.0f, .wrapLines = true,
            .enableMarkup = true, .horizontalAlign = ElementHorizontalAlign::Left, .verticalAlign = 0.0f});
        _log->setText("[color=\"#7fc8ff\"]Aria:[/color] Anyone up for the crypt run tonight?\n"
                      "[color=\"#ffb37a\"]Brom:[/color] Only if someone else carries the torches this time.\n"
                      "[color=\"#9ae08a\"]Cai:[/color] Meet at the gate in five. Bring potions!");

        // The input bar is split across the bottom of the window, like the title bar across its
        // top. Its send button is anchored to its right end, next to the grip in the window's corner
        ElementComponent* input = createElement(_chat->entity(), {.sprite = panel, .color = DARK,
            .anchor = Vector4(0.0f, 0.0f, 1.0f, 0.0f), .pivot = Vector2(0.5f, 0.0f)});
        input->setLeft(0.0f);
        input->setRight(0.0f);
        input->setHeight(56.0f);
        createElement(input->entity(), {.type = ElementType::Text, .color = MUTED,
                                        .anchor = Vector4(0.0f, 0.5f, 0.0f, 0.5f), .pivot = Vector2(0.0f, 0.5f),
                                        .text = "Say something…", .fontSize = 22})
            ->entity()->setLocalPosition(20.0f, 0.0f, 0.0f);
        ElementComponent* send = createElement(input->entity(), {.sprite = icons, .spriteFrame = 0,
            .color = Color(1.0f, 0.55f, 0.2f, 1.0f), .anchor = Vector4(1.0f, 0.5f, 1.0f, 0.5f),
            .pivot = Vector2(1.0f, 0.5f), .width = 36.0f, .height = 36.0f, .useInput = true});
        send->entity()->setLocalPosition(-52.0f, 0.0f, 0.0f);
        auto* sendButton = static_cast<ButtonComponent*>(send->entity()->addComponent<ButtonComponent>());
        sendButton->setImageEntity(send->entity());
        sendButton->setHitPadding(Vector4(12.0f, 12.0f, 12.0f, 12.0f));
        sendButton->setHoverTint(Color(1.0f, 0.7f, 0.45f, 1.0f));
        sendButton->on("click", [this]() {
            _log->setText(_log->text() + "\n[color=\"#ffcc00\"]You:[/color] " + _replies.front());
            _replies.push_back(_replies.front());
            _replies.pop_front();
        });

        // The grip is anchored to the window's bottom-right corner. Dragging it resizes the window:
        // once a press starts on an element, that element receives the moves until the press ends
        ElementComponent* grip = createElement(_chat->entity(), {.sprite = icons, .spriteFrame = 1, .color = MUTED,
            .anchor = Vector4(1.0f, 0.0f, 1.0f, 0.0f), .pivot = Vector2(1.0f, 0.0f), .width = 40.0f,
            .height = 40.0f, .useInput = true});
        for (const char* name : {"mousedown", "touchstart"}) {
            grip->on(name, [this](ElementInputEvent* event) {
                _drag = Drag{event->x, event->y, _chat->width(), _chat->height()};
            });
        }
        for (const char* name : {"mousemove", "touchmove"}) {
            grip->on(name, [this](ElementInputEvent* event) {
                if (_drag) {
                    // Events give the pointer in canvas points, which is the screen's resolution;
                    // the screen's scale converts the distance it moved to screen units
                    const float units = 1.0f / _screen->scale();
                    resizeChat(_drag->width + (event->x - _drag->x) * units,
                               _drag->height + (event->y - _drag->y) * units);
                }
            });
        }
        for (const char* name : {"mouseup", "touchend", "touchcancel"}) {
            grip->on(name, [this]() { _drag.reset(); });
        }

        layout();
        return true;
    }

    void update(float /*dt*/) override { layout(); }

private:
    struct Drag
    {
        float x;
        float y;
        float width;
        float height;
    };

    /// An element with this example's defaults for what a call leaves unset.
    ElementComponent* createElement(Entity* parent, const ElementProps& props) const
    {
        return visutwin::canvas::createElement(engine(), parent, props,
            {.type = ElementType::Image, .color = LIGHT, .font = _font});
    }

    // The window's size is limited by the screen's reference resolution, so that it stays on the
    // screen
    void resizeChat(const float width, const float height)
    {
        const Vector2& reference = _screen->referenceResolution();
        _chat->setWidth(std::clamp(width, 340.0f, reference.x - 80.0f));
        _chat->setHeight(std::clamp(height, 280.0f, reference.y - 150.0f));
    }

    // Use a portrait reference resolution on portrait canvases, where the window sits below the
    // score. A window sized in the other orientation can be too big for this one, so clamp its size
    // again
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
        resizeChat(_chat->width(), _chat->height());
        _chat->entity()->setLocalPosition(40.0f, portrait ? -110.0f : -40.0f, 0.0f);
    }

    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    std::unique_ptr<Asset> _landscapeTexture;
    FontResource* _font = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _chat = nullptr;
    ElementComponent* _log = nullptr;
    std::deque<std::string> _replies{"On my way!", "Wait for me at the bridge.", "I have the torches, promise."};
    std::optional<Drag> _drag;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(AnchorsExample)
