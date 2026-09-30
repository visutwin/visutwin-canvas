// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/text-auto-font-size.
//
// A multiplayer lobby whose name badges fit names of any length. Each badge is a text element
// with auto fit on, which shrinks its font until the name fits, between a maximum and a minimum
// size. Press Join to add players with longer names.
//
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
#include "framework/input/elementInput.h"
#include "scene/sprite.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

namespace
{
    const Color PANEL(0.16f, 0.18f, 0.23f, 1.0f);
    const Color PLATE(0.1f, 0.11f, 0.14f, 1.0f);
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);

    const std::array<const char*, 4> NAMES{{"Ada", "Maximilian von Hohenberg", "Grace", "xX_DragonSlayer_2026_Xx"}};

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
        Color color = LIGHT;
        std::optional<float> width;
        std::optional<float> height;
        bool useInput = false;
        FontResource* font = nullptr;
        std::string text;
        std::optional<int> fontSize;
    };
}

class TextAutoFontSizeExample final: public ExampleApp
{
public:
    TextAutoFontSizeExample()
        : ExampleApp({.title = "Text Auto Font Size", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

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
        Texture* atlasTexture = textureOf(*_uiAtlasTexture);
        if (!_font || !_bold || !atlasTexture) {
            spdlog::error("Failed to load the Roboto fonts or ui/ui-atlas.png");
            return false;
        }

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));

        _screenEntity = new Entity();
        _screenEntity->setEngine(engine());
        _screen = static_cast<ScreenComponent*>(_screenEntity->addComponent<ScreenComponent>());
        _screen->setScreenSpace(true);
        _screen->setReferenceResolution(Vector2(1280.0f, 720.0f));
        _screen->setScaleMode(ScreenScaleMode::Blend);
        _screen->setScaleBlend(0.5f);
        root()->addChild(_screenEntity);

        auto atlas = createUiAtlas(atlasTexture);
        _panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        auto outline = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel-outline"}, 2.0f,
                                                SpriteRenderMode::Sliced);
        std::vector<std::shared_ptr<Sprite>> avatars;
        for (int i = 1; i <= 4; ++i) {
            avatars.push_back(std::make_shared<Sprite>(atlas, std::vector<std::string>{"avatar-" + std::to_string(i)}));
        }

        _title = createElement(_screenEntity, {.type = ElementType::Text, .font = _bold, .text = "Lobby",
                                               .fontSize = 44});

        // The card of a player slot, empty until a player joins
        for (size_t index = 0; index < 4; ++index) {
            Slot slot;
            slot.card = createElement(_screenEntity, {.sprite = outline, .color = MUTED, .width = 232.0f,
                                                      .height = 250.0f});
            slot.avatar = createElement(slot.card->entity(), {.sprite = avatars[index], .width = 120.0f,
                                                              .height = 120.0f});
            slot.avatar->entity()->setLocalPosition(0.0f, 36.0f, 0.0f);
            createElement(slot.card->entity(), {.sprite = _panel, .color = PLATE, .width = 216.0f, .height = 48.0f})
                ->entity()->setLocalPosition(0.0f, -80.0f, 0.0f);

            // Shrink a name to fit its 200 x 40 badge, down to 12 units if needed
            slot.badge = createElement(slot.card->entity(), {.type = ElementType::Text, .font = _bold});
            slot.badge->setAutoWidth(false);
            slot.badge->setAutoHeight(false);
            slot.badge->setWidth(200.0f);
            slot.badge->setHeight(40.0f);
            slot.badge->setAutoFitWidth(true);
            slot.badge->setAutoFitHeight(true);
            slot.badge->setMaxFontSize(32);
            slot.badge->setMinFontSize(12);
            slot.badge->entity()->setLocalPosition(0.0f, -80.0f, 0.0f);

            // Empty slots show a muted placeholder
            slot.avatar->entity()->setEnabled(false);
            slot.badge->setText("Waiting…");
            slot.badge->setColor(MUTED);
            _slots.push_back(slot);
        }

        ElementComponent* button = createElement(_screenEntity, {.sprite = _panel,
            .color = Color(1.0f, 0.55f, 0.2f, 1.0f), .width = 220.0f, .height = 64.0f, .useInput = true});
        _button = button;
        auto* join = static_cast<ButtonComponent*>(button->entity()->addComponent<ButtonComponent>());
        join->setImageEntity(button->entity());
        join->setHoverTint(Color(1.0f, 0.7f, 0.45f, 1.0f));
        join->setPressedTint(Color(0.8f, 0.4f, 0.1f, 1.0f));
        join->setInactiveTint(Color(0.3f, 0.32f, 0.37f, 1.0f));
        ElementComponent* label = createElement(button->entity(), {.type = ElementType::Text,
            .color = Color(0.1f, 0.1f, 0.1f, 1.0f), .font = _bold, .text = "Join", .fontSize = 28});
        join->on("click", [this, join, label]() {
            joinPlayer();
            if (_players == _slots.size()) {
                join->setActive(false);
                label->setText("Lobby full");
            }
        });

        joinPlayer();
        joinPlayer();

        layout();
        return true;
    }

    void update(float /*dt*/) override { layout(); }

private:
    struct Slot
    {
        ElementComponent* card = nullptr;
        ElementComponent* badge = nullptr;
        ElementComponent* avatar = nullptr;
    };

    ElementComponent* createElement(Entity* parent, const ElementProps& props) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        ElementDesc desc{.type = props.type, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .pivot = Vector2(0.5f, 0.5f)};
        desc.width = props.width;
        desc.height = props.height;
        desc.useInput = props.useInput;
        element->setup(desc);
        if (props.sprite) {
            element->setSprite(props.sprite);
        }
        element->setColor(props.color);
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

    // A player joins the next empty slot, which turns into a filled card
    void joinPlayer()
    {
        const Slot& slot = _slots[_players];
        slot.card->setSprite(_panel);
        slot.card->setColor(PANEL);
        slot.avatar->entity()->setEnabled(true);
        slot.badge->setText(NAMES[_players]);
        slot.badge->setColor(LIGHT);
        ++_players;
    }

    // Four cards in a row on landscape canvases, and two rows of two on portrait ones
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
        for (size_t i = 0; i < _slots.size(); ++i) {
            const float fi = static_cast<float>(i);
            if (portrait) {
                _slots[i].card->entity()->setLocalPosition(static_cast<float>(i % 2) * 252.0f - 126.0f,
                                                           150.0f - std::floor(fi / 2.0f) * 274.0f, 0.0f);
            } else {
                _slots[i].card->entity()->setLocalPosition(fi * 252.0f - 378.0f, 20.0f, 0.0f);
            }
        }
        _title->entity()->setLocalPosition(0.0f, portrait ? 340.0f : 270.0f, 0.0f);
        _button->entity()->setLocalPosition(0.0f, portrait ? -322.0f : -210.0f, 0.0f);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    FontResource* _bold = nullptr;
    std::shared_ptr<Sprite> _panel;
    Entity* _screenEntity = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _title = nullptr;
    ElementComponent* _button = nullptr;
    std::vector<Slot> _slots;
    size_t _players = 0;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(TextAutoFontSizeExample)
