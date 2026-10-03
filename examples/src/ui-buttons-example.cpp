// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 29.09.2026
//
// Port of upstream user-interface/buttons.
//
// A game's main menu built from button components. Play and Continue tint as they are
// hovered and pressed, and Continue stays inactive until there is a game to continue.
// Options and Credits show sprite frames instead, and the close button has a larger hit
// area. A pressed button sinks slightly, and the cursor turns into a pointer over a button.
//
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL_mouse.h>

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
    const Color INK(0.1f, 0.1f, 0.1f, 1.0f);
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);

    /// The button component properties that this example sets.
    struct ButtonProps
    {
        ButtonTransitionMode transitionMode = ButtonTransitionMode::Tint;
        Color hoverTint = Color(0.75f, 0.75f, 0.75f, 1.0f);
        Color pressedTint = Color(0.5f, 0.5f, 0.5f, 1.0f);
        Color inactiveTint = Color(0.25f, 0.25f, 0.25f, 1.0f);
        float fadeDuration = 0.0f;
        bool active = true;
        int hoverSpriteFrame = 0;
        int pressedSpriteFrame = 0;
        int inactiveSpriteFrame = 0;
    };
}

class UiButtonsExample final: public ExampleApp
{
public:
    UiButtonsExample()
        : ExampleApp({.title = "UI Buttons", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

    ~UiButtonsExample() override
    {
        SDL_SetCursor(SDL_GetDefaultCursor());
        if (_pointer) {
            SDL_DestroyCursor(_pointer);
        }
    }

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
        _pointer = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));

        _screen = createScreen();
        Entity* screenEntity = _screen->entity();
        _screenEntity = screenEntity;

        // Sliced sprites from the UI kit (ui-atlas.mjs). The atlas is drawn at 2 pixels per
        // screen unit
        auto atlas = std::make_shared<TextureAtlas>();
        atlas->setTexture(atlasTexture);
        const Vector4 buttonBorder(32.0f, 44.0f, 32.0f, 32.0f);
        atlas->setFrame("panel", {.rect = Vector4(548.0f, 660.0f, 128.0f, 128.0f), .pivot = Vector2(0.5f, 0.5f),
                                  .border = Vector4(32.0f, 32.0f, 32.0f, 32.0f)});
        atlas->setFrame("button", {.rect = Vector4(780.0f, 892.0f, 128.0f, 128.0f), .pivot = Vector2(0.5f, 0.5f),
                                   .border = buttonBorder});
        atlas->setFrame("button-hover", {.rect = Vector4(4.0f, 660.0f, 128.0f, 128.0f),
                                         .pivot = Vector2(0.5f, 0.5f), .border = buttonBorder});
        atlas->setFrame("button-pressed", {.rect = Vector4(276.0f, 660.0f, 128.0f, 128.0f),
                                           .pivot = Vector2(0.5f, 0.5f), .border = buttonBorder});
        atlas->setFrame("button-inactive", {.rect = Vector4(140.0f, 660.0f, 128.0f, 128.0f),
                                            .pivot = Vector2(0.5f, 0.5f), .border = buttonBorder});
        atlas->setFrame("icon-close", {.rect = Vector4(316.0f, 556.0f, 96.0f, 96.0f), .pivot = Vector2(0.5f, 0.5f),
                                       .border = Vector4(0.0f, 0.0f, 0.0f, 0.0f)});
        auto rounded = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f,
                                                SpriteRenderMode::Sliced);
        // One sprite with a frame for each state of a sprite change button
        auto states = std::make_shared<Sprite>(
            atlas, std::vector<std::string>{"button", "button-hover", "button-pressed", "button-inactive"}, 2.0f,
            SpriteRenderMode::Sliced);
        auto cross = std::make_shared<Sprite>(atlas, std::vector<std::string>{"icon-close"});

        ElementComponent* title = createText(screenEntity, "STARFALL", _bold, 80, LIGHT);
        title->setSpacing(1.15f);
        title->entity()->setLocalPosition(0.0f, 230.0f, 0.0f);

        // Tint buttons: the tints replace the image's color, so they are lighter and darker
        // oranges
        const ButtonProps tints{.hoverTint = Color(1.0f, 0.7f, 0.45f, 1.0f),
                                .pressedTint = Color(0.8f, 0.4f, 0.1f, 1.0f),
                                .inactiveTint = Color(0.3f, 0.32f, 0.37f, 1.0f)};
        // DEVIATION: no tint fade, where upstream's example fades over 100 ms.
        ButtonComponent* play = createButton("Play", 100.0f, rounded, ORANGE, tints);
        ButtonProps inactiveTints = tints;
        inactiveTints.active = false;
        ButtonComponent* resume = createButton("Continue", 12.0f, rounded, ORANGE, inactiveTints);

        // Sprite change buttons: each state shows another frame of the image's sprite
        const ButtonProps frames{.transitionMode = ButtonTransitionMode::SpriteChange, .hoverSpriteFrame = 1,
                                 .pressedSpriteFrame = 2, .inactiveSpriteFrame = 3};
        ButtonComponent* options = createButton("Options", -76.0f, states, std::nullopt, frames);
        ButtonComponent* credits = createButton("Credits", -164.0f, states, std::nullopt, frames);

        // A small close button in the corner, whose hit area reaches 16 units beyond its image
        auto* closeEntity = new Entity();
        closeEntity->setEngine(engine());
        auto* closeImage = static_cast<ElementComponent*>(closeEntity->addComponent<ElementComponent>());
        closeImage->setup({.type = ElementType::Image, .anchor = Vector4(1.0f, 1.0f, 1.0f, 1.0f),
                           .pivot = Vector2(1.0f, 1.0f), .width = 32.0f, .height = 32.0f, .useInput = true});
        closeImage->setSprite(cross);
        closeImage->setColor(MUTED);
        auto* close = static_cast<ButtonComponent*>(closeEntity->addComponent<ButtonComponent>());
        close->setImageEntity(closeEntity);
        close->setHitPadding(Vector4(16.0f, 16.0f, 16.0f, 16.0f));
        close->setHoverTint(LIGHT);
        close->setPressedTint(ORANGE);
        closeEntity->setLocalPosition(-40.0f, -40.0f, 0.0f);
        screenEntity->addChild(closeEntity);
        followCursor(close);

        _status = createText(screenEntity, "Start a new game to unlock Continue", _font, 24, MUTED);
        _status->entity()->setLocalPosition(0.0f, -264.0f, 0.0f);

        play->on("click", [this, resume]() {
            // there is a saved game now, so Continue becomes active
            resume->setActive(true);
            say("A new game begins");
        });
        resume->on("click", [this]() { say("Continuing where you left off"); });
        options->on("click", [this]() { say("Options"); });
        credits->on("click", [this]() { say("Credits"); });
        close->on("click", [this]() { say("Thanks for playing!"); });

        layout();
        return true;
    }

    void update(float /*dt*/) override
    {
        layout();
    }

private:
    /// A text element centred on its parent.
    ElementComponent* createText(Entity* parent, const std::string& text, FontResource* font, const int fontSize,
                                 const Color& color) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        element->setup({.type = ElementType::Text, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
                        .pivot = Vector2(0.5f, 0.5f)});
        element->setFontResource(font);
        element->setFontSize(fontSize);
        element->setColor(color);
        element->setText(text);
        parent->addChild(entity);
        return element;
    }

    /// Create a menu button: an image element that receives input, a button component that
    /// changes the image as the button is hovered and pressed, and a label without input, so
    /// that clicks on the label go to the button.
    ButtonComponent* createButton(const std::string& text, const float y, const std::shared_ptr<Sprite>& sprite,
                                  const std::optional<Color>& color, const ButtonProps& props)
    {
        auto* buttonEntity = new Entity();
        buttonEntity->setEngine(engine());
        auto* image = static_cast<ElementComponent*>(buttonEntity->addComponent<ElementComponent>());
        image->setup({.type = ElementType::Image, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
                      .pivot = Vector2(0.5f, 0.5f), .width = 260.0f, .height = 72.0f, .useInput = true});
        image->setSprite(sprite);
        if (color) {
            image->setColor(*color);
        }
        auto* button = static_cast<ButtonComponent*>(buttonEntity->addComponent<ButtonComponent>());
        button->setImageEntity(buttonEntity);
        button->setTransitionMode(props.transitionMode);
        button->setHoverTint(props.hoverTint);
        button->setPressedTint(props.pressedTint);
        button->setInactiveTint(props.inactiveTint);
        button->setFadeDuration(props.fadeDuration);
        button->setActive(props.active);
        button->setHoverSpriteFrame(props.hoverSpriteFrame);
        button->setPressedSpriteFrame(props.pressedSpriteFrame);
        button->setInactiveSpriteFrame(props.inactiveSpriteFrame);
        buttonEntity->setLocalPosition(0.0f, y, 0.0f);
        _screenEntity->addChild(buttonEntity);

        // dark text on the orange tint buttons, light text on the slate sprite frames
        createText(buttonEntity, text, _bold, 30,
                   props.transitionMode == ButtonTransitionMode::SpriteChange ? LIGHT : INK);

        // Sink the button slightly while it is pressed, and show a pointer while it is hovered
        button->on("pressedstart", [buttonEntity]() { buttonEntity->setLocalScale(0.95f, 0.95f, 1.0f); });
        button->on("pressedend", [buttonEntity]() { buttonEntity->setLocalScale(1.0f, 1.0f, 1.0f); });
        followCursor(button);
        return button;
    }

    void followCursor(ButtonComponent* button)
    {
        button->on("hoverstart", [this]() { SDL_SetCursor(_pointer); });
        button->on("hoverend", []() { SDL_SetCursor(SDL_GetDefaultCursor()); });
    }

    void say(const std::string& text) { _status->setText(text); }

    // Use a portrait reference resolution on portrait canvases, and scale to whichever axis
    // has less room, so the whole menu stays on screen
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
    }

    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    FontResource* _bold = nullptr;
    SDL_Cursor* _pointer = nullptr;
    Entity* _screenEntity = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _status = nullptr;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(UiButtonsExample)
