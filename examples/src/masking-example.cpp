// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// Port of upstream user-interface/masking.
//
// A player profile card that uses masks three ways. The cover photo pans inside a rectangle
// mask, a shaped mask clips the avatar to a circle (the transparent corners of the circle
// sprite are outside it), and the card's own rounded mask clips both, so the masks nest. A
// mask is not drawn itself, so the card's colour comes from an image that fills it.
//
#include <array>
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
    const Color PANEL(0.16f, 0.18f, 0.23f, 1.0f);
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);

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

    /// The element properties this example sets; centred on the parent unless
    /// they say otherwise.
    struct ElementProps
    {
        ElementType type = ElementType::Image;
        std::shared_ptr<Sprite> sprite;
        Texture* texture = nullptr;
        Color color = LIGHT;
        Vector4 anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f);
        Vector2 pivot = Vector2(0.5f, 0.5f);
        std::optional<Vector4> margin;
        std::optional<float> width;
        std::optional<float> height;
        bool mask = false;
        FontResource* font = nullptr;
        std::string text;
        int fontSize = 32;
    };
}

class MaskingExample final: public ExampleApp
{
public:
    MaskingExample()
        : ExampleApp({.title = "Masking", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

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
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _boldAsset = std::make_unique<Asset>("bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        _landscapeTexture = std::make_unique<Asset>("landscape", AssetType::TEXTURE, assetPath("ui/landscape.png"),
            AssetData{.mipmaps = true});
        _font = fontOf(*_fontAsset);
        FontResource* bold = fontOf(*_boldAsset);
        Texture* atlasTexture = textureOf(*_uiAtlasTexture);
        Texture* landscape = textureOf(*_landscapeTexture);
        if (!_font || !bold || !atlasTexture || !landscape) {
            spdlog::error("Failed to load the Roboto fonts, ui/ui-atlas.png or ui/landscape.png");
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

        // Frames of the UI kit atlas
        auto atlas = std::make_shared<TextureAtlas>();
        atlas->setTexture(atlasTexture);
        const Vector2 centre(0.5f, 0.5f);
        const Vector4 none(0.0f, 0.0f, 0.0f, 0.0f);
        atlas->setFrame("panel", {.rect = Vector4(548.0f, 660.0f, 128.0f, 128.0f), .pivot = centre,
                                  .border = Vector4(32.0f, 32.0f, 32.0f, 32.0f)});
        atlas->setFrame("circle", {.rect = Vector4(412.0f, 660.0f, 128.0f, 128.0f), .pivot = centre, .border = none});
        atlas->setFrame("avatar-2", {.rect = Vector4(372.0f, 892.0f, 128.0f, 128.0f), .pivot = centre, .border = none});
        auto rounded = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        auto circle = std::make_shared<Sprite>(atlas, std::vector<std::string>{"circle"});
        auto portrait = std::make_shared<Sprite>(atlas, std::vector<std::string>{"avatar-2"});

        // The card is a mask, so its rounded shape clips everything below it. A mask is not
        // drawn itself, so the card's color comes from an image that fills it
        ElementComponent* card = createElement(screenEntity, {.sprite = rounded, .width = 360.0f, .height = 470.0f,
                                                              .mask = true});
        createElement(card->entity(), {.color = PANEL, .anchor = Vector4(0.0f, 0.0f, 1.0f, 1.0f),
                                       .margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f)});

        // The cover is a rectangle mask across the top of the card, over a larger photo that
        // pans inside it. The photo is clipped by both masks, so its top corners follow the
        // rounded card
        ElementComponent* cover = createElement(card->entity(), {.anchor = Vector4(0.0f, 1.0f, 1.0f, 1.0f),
                                                                 .pivot = Vector2(0.5f, 1.0f), .mask = true});
        cover->setLeft(0.0f);
        cover->setRight(0.0f);
        cover->setHeight(200.0f);
        _photo = createElement(cover->entity(), {.texture = landscape, .width = 400.0f, .height = 400.0f});

        // The avatar is a shaped mask: the transparent corners of the circle sprite clip the
        // square picture below it into a circle. A ring behind it cuts it out of the cover
        createElement(card->entity(), {.sprite = circle, .color = PANEL, .width = 132.0f, .height = 132.0f})
            ->entity()->setLocalPosition(0.0f, 35.0f, 0.0f);
        ElementComponent* avatar = createElement(card->entity(), {.sprite = circle, .width = 116.0f, .height = 116.0f,
                                                                  .mask = true});
        avatar->entity()->setLocalPosition(0.0f, 35.0f, 0.0f);
        createElement(avatar->entity(), {.sprite = portrait, .width = 116.0f, .height = 116.0f});
        createElement(card->entity(), {.sprite = circle, .color = Color(0.3f, 0.85f, 0.45f, 1.0f), .width = 24.0f,
                                       .height = 24.0f})
            ->entity()->setLocalPosition(40.0f, -5.0f, 0.0f);

        createElement(card->entity(), {.type = ElementType::Text, .font = bold, .text = "Aria Nightfall",
                                       .fontSize = 34})
            ->entity()->setLocalPosition(0.0f, -70.0f, 0.0f);
        createElement(card->entity(), {.type = ElementType::Text, .color = MUTED, .text = "Level 42 · Ranger",
                                       .fontSize = 24})
            ->entity()->setLocalPosition(0.0f, -108.0f, 0.0f);

        const std::array<std::array<const char*, 2>, 3> stats{{{"318", "Wins"}, {"#12", "Rank"}, {"96h", "Played"}}};
        for (size_t i = 0; i < stats.size(); ++i) {
            const float x = (static_cast<float>(i) - 1.0f) * 110.0f;
            createElement(card->entity(), {.type = ElementType::Text, .font = bold, .text = stats[i][0], .fontSize = 30})
                ->entity()->setLocalPosition(x, -168.0f, 0.0f);
            createElement(card->entity(), {.type = ElementType::Text, .color = MUTED, .text = stats[i][1],
                                           .fontSize = 20})
                ->entity()->setLocalPosition(x, -200.0f, 0.0f);
        }

        layout();
        return true;
    }

    // Drift the photo, so the cover shows a different part of it
    void update(const float dt) override
    {
        layout();
        _time += dt;
        _photo->entity()->setLocalPosition(std::sin(_time * 0.4f) * 20.0f, std::sin(_time * 0.25f) * 90.0f, 0.0f);
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
        element->setup(desc);
        if (props.sprite) {
            element->setSprite(props.sprite);
        }
        if (props.texture) {
            element->setTexture(props.texture);
        }
        element->setColor(props.color);
        element->setMask(props.mask);
        if (props.type == ElementType::Text) {
            element->setFontResource(props.font ? props.font : _font);
            element->setFontSize(props.fontSize);
            element->setText(props.text);
        }
        parent->addChild(entity);
        return element;
    }

    // Use a portrait reference resolution on portrait canvases, and scale to whichever axis
    // has less room, so the whole card stays on screen
    void layout()
    {
        const auto [w, h] = engine()->canvasSize();
        if (w == _laidOutWidth && h == _laidOutHeight) {
            return;
        }
        _laidOutWidth = w;
        _laidOutHeight = h;
        const Vector2 reference = h > w ? Vector2(540.0f, 960.0f) : Vector2(1280.0f, 720.0f);
        _screen->setReferenceResolution(reference);
        _screen->setScaleBlend(static_cast<float>(w) / reference.x > static_cast<float>(h) / reference.y ? 1.0f : 0.0f);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    std::unique_ptr<Asset> _landscapeTexture;
    FontResource* _font = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _photo = nullptr;
    float _time = 0.0f;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(MaskingExample)
