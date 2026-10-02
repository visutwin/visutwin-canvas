// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// Port of upstream user-interface/image-fit.
//
// A level select screen whose art comes in four shapes. The cards Cover their squares, cropped
// by a mask, and each card's pivot picks the part that stays in view. The preview shows the
// whole picture with Contain, and the map shows part of a texture with rect. Tap a card.
//
#include <algorithm>
#include <array>
#include <cmath>
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
    const Color ORANGE(1.0f, 0.55f, 0.2f, 1.0f);
    const Color PANEL(0.16f, 0.18f, 0.23f, 1.0f);
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);

    struct Level
    {
        const char* name;
        const char* file;
        Vector2 pivot;
        Vector2 map;
    };

    const std::array<Level, 4> LEVELS{{
        {"Sunken Crypt", "ui/level-crypt.png", Vector2(0.5f, 0.0f), Vector2(0.71f, 0.31f)},
        {"Emerald Forest", "ui/level-forest.png", Vector2(0.8f, 0.5f), Vector2(0.27f, 0.37f)},
        {"Dune Sea", "ui/level-desert.png", Vector2(0.7f, 0.5f), Vector2(0.7f, 0.68f)},
        {"Frost Peak", "ui/level-peak.png", Vector2(0.5f, 0.5f), Vector2(0.34f, 0.68f)},
    }};
}

class ImageFitExample final: public ExampleApp
{
public:
    ImageFitExample()
        : ExampleApp({.title = "Image Fit", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options, {.button = true});
    }

    bool create() override
    {
        _boldAsset = std::make_unique<Asset>("bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        _mapTexture = std::make_unique<Asset>("map", AssetType::TEXTURE, assetPath("ui/world-map.png"));
        _bold = _boldAsset->resourceAs<FontResource>();
        Texture* atlasTexture = _uiAtlasTexture->resourceAs<Texture>();
        Texture* mapTexture = _mapTexture->resourceAs<Texture>();
        for (const Level& level : LEVELS) {
            _artAssets.push_back(std::make_unique<Asset>(level.name, AssetType::TEXTURE, assetPath(level.file)));
            _art.push_back(_artAssets.back()->resourceAs<Texture>());
        }
        if (!_bold || !atlasTexture || !mapTexture || std::find(_art.begin(), _art.end(), nullptr) != _art.end()) {
            spdlog::error("Failed to load the Roboto font or the ui textures");
            return false;
        }

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));

        _screen = createScreen();
        Entity* screenEntity = _screen->entity();

        auto atlas = createUiAtlas(atlasTexture);
        auto panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        auto outline = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel-outline"}, 2.0f,
                                                SpriteRenderMode::Sliced);
        auto circle = std::make_shared<Sprite>(atlas, std::vector<std::string>{"circle"});
        const Vector4 fill(0.0f, 0.0f, 1.0f, 1.0f);
        const Vector4 zero(0.0f, 0.0f, 0.0f, 0.0f);

        _title = createElement(screenEntity, {.type = ElementType::Text, .text = "Choose a level", .fontSize = 40});

        // The preview: the whole of the level's art, contained in a 16:9 frame, whatever its shape
        _preview = createElement(screenEntity, {.sprite = panel, .color = PANEL, .width = 560.0f, .height = 315.0f});
        _picture = createElement(_preview->entity(), {.fitMode = ElementFitMode::Contain, .anchor = fill,
                                                      .margin = Vector4(12.0f, 12.0f, 12.0f, 12.0f)});
        _caption = createElement(_preview->entity(), {.type = ElementType::Text, .anchor = zero,
                                                      .pivot = Vector2(0.0f, 1.0f), .fontSize = 32});
        _caption->entity()->setLocalPosition(4.0f, -16.0f, 0.0f);

        // The map: a round mask over the world map, which shows the part of it around the level.
        // The map is 4:3, so a part 0.3 of its width and 0.4 of its height is square
        _inset = createElement(_preview->entity(), {.sprite = circle, .anchor = Vector4(1.0f, 0.0f, 1.0f, 0.0f),
                                                    .width = 150.0f, .height = 150.0f, .mask = true});
        _map = createElement(_inset->entity(), {.texture = mapTexture, .anchor = fill, .margin = zero});
        createElement(_inset->entity(), {.sprite = circle, .color = ORANGE, .width = 18.0f, .height = 18.0f});

        // The cards: each level's art covers a square, and the card, a mask, crops what overflows.
        // The art is placed at its pivot: the crypt keeps its door in view, and the dunes their
        // pyramid
        for (size_t i = 0; i < LEVELS.size(); ++i) {
            ElementComponent* card = createElement(screenEntity, {.sprite = panel, .width = 170.0f, .height = 170.0f,
                                                                  .useInput = true, .mask = true});
            ElementComponent* cover = createElement(card->entity(), {.texture = _art[i],
                .fitMode = ElementFitMode::Cover, .anchor = fill, .pivot = LEVELS[i].pivot, .margin = zero});

            // A mask isn't drawn, so the button tints the art in it
            auto* button = static_cast<ButtonComponent*>(card->entity()->addComponent<ButtonComponent>());
            button->setImageEntity(cover->entity());
            button->setHoverTint(Color(0.8f, 0.82f, 0.86f, 1.0f));
            button->setPressedTint(Color(0.6f, 0.62f, 0.66f, 1.0f));
            button->on("click", [this, i]() { choose(i); });
            _cards.push_back(card);
        }

        // The ring around the chosen card is drawn over the cards, so it is not cropped by their
        // masks
        _ring = createElement(screenEntity, {.sprite = outline, .color = ORANGE, .width = 186.0f, .height = 186.0f});

        layout();
        return true;
    }

    void update(float /*dt*/) override { layout(); }

private:
    /// An element with this example's defaults for what a call leaves unset.
    ElementComponent* createElement(Entity* parent, const ElementProps& props) const
    {
        return visutwin::canvas::createElement(engine(), parent, props, {.type = ElementType::Image,
            .color = LIGHT, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .pivot = Vector2(0.5f, 0.5f),
            .font = _bold});
    }

    // Choosing a level shows its art and name, and moves the map to the level and the ring to its
    // card
    void choose(const size_t i)
    {
        _chosen = i;
        const Level& level = LEVELS[i];
        _picture->setTexture(_art[i]);
        _caption->setText(level.name);
        _map->setRect(Vector4(level.map.x - 0.15f, level.map.y - 0.2f, 0.3f, 0.4f));
        _ring->entity()->setLocalPosition(_cards[i]->entity()->localPosition());
    }

    // The cards in a row under the preview on landscape canvases, and in a square on portrait ones
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
        _title->entity()->setLocalPosition(0.0f, portrait ? 390.0f : 305.0f, 0.0f);
        _preview->setWidth(portrait ? 500.0f : 560.0f);
        _preview->setHeight(portrait ? 281.0f : 315.0f);
        _preview->entity()->setLocalPosition(0.0f, portrait ? 190.0f : 100.0f, 0.0f);
        _inset->entity()->setLocalPosition(portrait ? -85.0f : -20.0f, portrait ? 85.0f : 20.0f, 0.0f);
        for (size_t i = 0; i < _cards.size(); ++i) {
            const float fi = static_cast<float>(i);
            const float x = portrait ? (static_cast<float>(i % 2) - 0.5f) * 200.0f : (fi - 1.5f) * 200.0f;
            const float y = portrait ? -130.0f - std::floor(fi / 2.0f) * 200.0f : -235.0f;
            _cards[i]->entity()->setLocalPosition(x, y, 0.0f);
        }
        choose(_chosen);
    }

    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    std::unique_ptr<Asset> _mapTexture;
    std::vector<std::unique_ptr<Asset>> _artAssets;
    std::vector<Texture*> _art;
    FontResource* _bold = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _title = nullptr;
    ElementComponent* _preview = nullptr;
    ElementComponent* _picture = nullptr;
    ElementComponent* _caption = nullptr;
    ElementComponent* _inset = nullptr;
    ElementComponent* _map = nullptr;
    ElementComponent* _ring = nullptr;
    std::vector<ElementComponent*> _cards;
    size_t _chosen = 0;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(ImageFitExample)
