// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 29.09.2026
//
// Port of upstream user-interface/panel.
//
// A quest dialog, achievement toasts and a quest log, all drawn from small sprites of the UI
// kit: SLICED panels keep their corners as they stretch to any size, like the toasts that fit
// their text, and the TILED paper of the log repeats its ruled middle as the log grows.
// Accept a quest to add it to the log and slide a toast down from the top; Decline skips to
// the next quest.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
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
    const Color PANEL(0.16f, 0.18f, 0.23f, 1.0f);
    const Color INK(0.12f, 0.13f, 0.16f, 1.0f);
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);
    const Color SLATE(0.26f, 0.29f, 0.36f, 1.0f);

    struct Quest
    {
        const char* name;
        const char* description;
    };
    const std::array<Quest, 3> QUESTS{{
        {"A Lost Heirloom", "The innkeeper lost her ring near the old mill. Will you look for it?"},
        {"Wolves at the Gate", "Wolves prowl the east road. Drive them off before nightfall."},
        {"The Silent Bell", "The chapel bell has not rung for a week. Find out why."},
    }};
}

class UiPanelExample final: public ExampleApp
{
public:
    UiPanelExample()
        : ExampleApp({.title = "UI Panel", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

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
        _font = _fontAsset->resourceAs<FontResource>();
        _bold = _boldAsset->resourceAs<FontResource>();
        Texture* atlasTexture = _uiAtlasTexture->resourceAs<Texture>();
        if (!_font || !_bold || !atlasTexture) {
            spdlog::error("Failed to load the Roboto fonts or ui/ui-atlas.png");
            return false;
        }

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));

        _screen = createScreen();
        Entity* screenEntity = _screen->entity();

        // Sprites from the UI kit's texture atlas (ui-atlas.mjs). Its panel frame is 128 x 128
        // pixels with 32 pixel borders, so at 2 pixels per unit the corners stay 16 units wide,
        // whatever the size of the panel
        auto atlas = std::make_shared<TextureAtlas>();
        atlas->setTexture(atlasTexture);
        const Vector2 centre(0.5f, 0.5f);
        const Vector4 border32(32.0f, 32.0f, 32.0f, 32.0f);
        atlas->setFrame("panel", {.rect = Vector4(548.0f, 660.0f, 128.0f, 128.0f), .pivot = centre, .border = border32});
        atlas->setFrame("shadow", {.rect = Vector4(4.0f, 796.0f, 224.0f, 224.0f), .pivot = centre,
                                   .border = Vector4(80.0f, 80.0f, 80.0f, 80.0f)});
        atlas->setFrame("paper", {.rect = Vector4(820.0f, 660.0f, 128.0f, 128.0f), .pivot = centre, .border = border32});
        atlas->setFrame("icon-star", {.rect = Vector4(524.0f, 348.0f, 96.0f, 96.0f), .pivot = centre,
                                      .border = Vector4(0.0f, 0.0f, 0.0f, 0.0f)});
        const auto sprite = [&atlas](const std::string& frame, const SpriteRenderMode mode) {
            return std::make_shared<Sprite>(atlas, std::vector<std::string>{frame}, 2.0f, mode);
        };
        auto panelSprite = sprite("panel", SpriteRenderMode::Sliced);
        auto shadowSprite = sprite("shadow", SpriteRenderMode::Sliced);
        auto paperSprite = sprite("paper", SpriteRenderMode::Tiled);
        auto starSprite = sprite("icon-star", SpriteRenderMode::Simple);

        // The dialog: one sliced sprite, stretched to 400 x 240, over a soft sliced shadow
        _shadow = createElement(screenEntity, {.sprite = shadowSprite, .color = Color(0.0f, 0.0f, 0.0f, 1.0f),
                                               .width = 448.0f, .height = 288.0f});
        _dialog = createElement(screenEntity, {.sprite = panelSprite, .color = PANEL, .width = 400.0f,
                                               .height = 240.0f});

        // Its title and description, anchored to the top-left corner of the dialog
        ElementProps topLeft{.type = ElementType::Text, .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f),
                             .pivot = Vector2(0.0f, 1.0f), .width = 352.0f, .autoWidth = false,
                             .horizontalAlign = ElementHorizontalAlign::Left, .verticalAlign = 1.0f};
        ElementProps titleProps = topLeft;
        titleProps.font = _bold;
        titleProps.fontSize = 28;
        _title = createElement(_dialog->entity(), titleProps);
        ElementProps bodyProps = topLeft;
        bodyProps.fontSize = 22;
        bodyProps.lineHeight = 28.0f;
        bodyProps.color = MUTED;
        bodyProps.wrapLines = true;
        _body = createElement(_dialog->entity(), bodyProps);
        _title->entity()->setLocalPosition(24.0f, -22.0f, 0.0f);
        _body->entity()->setLocalPosition(24.0f, -66.0f, 0.0f);

        ButtonComponent* decline = createButton("Decline", -90.0f,
            {SLATE, Color(0.33f, 0.37f, 0.45f, 1.0f), Color(0.2f, 0.22f, 0.28f, 1.0f)}, LIGHT, panelSprite);
        ButtonComponent* accept = createButton("Accept", 90.0f,
            {ORANGE, Color(1.0f, 0.7f, 0.45f, 1.0f), Color(0.8f, 0.4f, 0.1f, 1.0f)}, INK, panelSprite);

        // A toast, sized to its message: only the middle of the sliced panel grows
        _toast = createElement(screenEntity, {.sprite = panelSprite, .color = PANEL,
                                              .anchor = Vector4(0.5f, 1.0f, 0.5f, 1.0f), .pivot = Vector2(0.5f, 1.0f),
                                              .height = 64.0f});
        createElement(_toast->entity(), {.sprite = starSprite, .color = ORANGE,
                                         .anchor = Vector4(0.0f, 0.5f, 0.0f, 0.5f), .width = 32.0f, .height = 32.0f})
            ->entity()->setLocalPosition(38.0f, 0.0f, 0.0f);
        _message = createElement(_toast->entity(), {.type = ElementType::Text,
                                                    .anchor = Vector4(0.0f, 0.5f, 0.0f, 0.5f),
                                                    .pivot = Vector2(0.0f, 0.5f), .fontSize = 24});
        _message->entity()->setLocalPosition(66.0f, 0.0f, 0.0f);

        // The quest log: ruled paper whose tiled middle repeats as the log grows
        _log = createElement(screenEntity, {.sprite = paperSprite, .color = Color(1.0f, 1.0f, 1.0f, 1.0f),
                                            .pivot = Vector2(0.5f, 1.0f), .width = 300.0f, .height = 96.0f});
        createElement(_log->entity(), {.type = ElementType::Text, .color = INK,
                                       .anchor = Vector4(0.5f, 1.0f, 0.5f, 1.0f), .pivot = Vector2(0.5f, 1.0f),
                                       .font = _bold, .text = "Quest Log", .fontSize = 26})
            ->entity()->setLocalPosition(0.0f, -18.0f, 0.0f);
        logQuest("The Blacksmith’s Hammer");

        accept->on("click", [this] {
            const std::string name = QUESTS[_quest].name;
            logQuest(name);
            _message->setText("Quest accepted: " + name);
            _toast->setWidth(_message->width() + 96.0f);
            _toastTime = 0.0f;
            nextQuest();
        });
        decline->on("click", [this] { nextQuest(); });

        layout();
        nextQuest();
        return true;
    }

    // Slide the toast down from the top of the screen, and back up after a couple of seconds
    void update(const float dt) override
    {
        layout();
        _toastTime += dt;
        const float shown = std::min({_toastTime / 0.3f, 1.0f, std::max((2.8f - _toastTime) / 0.3f, 0.0f)});
        _toast->entity()->setLocalPosition(0.0f, 80.0f - 104.0f * (1.0f - std::pow(1.0f - shown, 3.0f)), 0.0f);
    }

private:
    /// An element with this example's defaults for what a call leaves unset.
    ElementComponent* createElement(Entity* parent, const ElementProps& props) const
    {
        return visutwin::canvas::createElement(engine(), parent, props, {.type = ElementType::Image,
            .color = LIGHT, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .pivot = Vector2(0.5f, 0.5f),
            .font = _font});
    }

    /// One of the dialog's buttons, a sliced sprite as well.
    ButtonComponent* createButton(const std::string& label, const float x, const std::array<Color, 3>& colors,
                                  const Color& labelColor, const std::shared_ptr<Sprite>& sprite)
    {
        ElementComponent* image = createElement(_dialog->entity(), {.sprite = sprite, .color = colors[0],
            .anchor = Vector4(0.5f, 0.0f, 0.5f, 0.0f), .pivot = Vector2(0.5f, 0.0f), .width = 164.0f,
            .height = 52.0f, .useInput = true});
        image->entity()->setLocalPosition(x, 22.0f, 0.0f);
        auto* button = static_cast<ButtonComponent*>(image->entity()->addComponent<ButtonComponent>());
        button->setImageEntity(image->entity());
        button->setHoverTint(colors[1]);
        button->setPressedTint(colors[2]);
        createElement(image->entity(), {.type = ElementType::Text, .color = labelColor, .font = _bold, .text = label,
                                        .fontSize = 24});
        return button;
    }

    // Write a quest in the log, which grows by a line, keeping the last five
    void logQuest(const std::string& name)
    {
        if (_entries.size() == 5) {
            Entity* oldest = _entries.front()->entity();
            _entries.pop_front();
            oldest->destroy();
            auto removed = oldest->remove();
        }
        _entries.push_back(createElement(_log->entity(), {.type = ElementType::Text, .color = INK,
            .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f), .pivot = Vector2(0.0f, 0.5f), .text = "• " + name,
            .fontSize = 22}));
        for (size_t i = 0; i < _entries.size(); ++i) {
            _entries[i]->entity()->setLocalPosition(28.0f, -80.0f - static_cast<float>(i) * 32.0f, 0.0f);
        }
        _log->setHeight(96.0f + static_cast<float>(_entries.size()) * 32.0f);
    }

    void nextQuest()
    {
        _quest = (_quest + 1) % QUESTS.size();
        _title->setText(QUESTS[_quest].name);
        _body->setText(QUESTS[_quest].description);
    }

    // Side by side on landscape canvases, and stacked on portrait ones
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
        const float dialogX = portrait ? 0.0f : -150.0f;
        const float dialogY = portrait ? 190.0f : 0.0f;
        _dialog->entity()->setLocalPosition(dialogX, dialogY, 0.0f);
        _shadow->entity()->setLocalPosition(dialogX, dialogY - 8.0f, 0.0f);
        _log->entity()->setLocalPosition(portrait ? 0.0f : 230.0f, portrait ? 30.0f : 120.0f, 0.0f);
    }

    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    FontResource* _bold = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _shadow = nullptr;
    ElementComponent* _dialog = nullptr;
    ElementComponent* _title = nullptr;
    ElementComponent* _body = nullptr;
    ElementComponent* _toast = nullptr;
    ElementComponent* _message = nullptr;
    ElementComponent* _log = nullptr;
    std::deque<ElementComponent*> _entries;
    size_t _quest = QUESTS.size() - 1;   // nextQuest() moves on to the first
    float _toastTime = 1e9f;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(UiPanelExample)
