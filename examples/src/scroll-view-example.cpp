// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/scroll-view.
//
// A leaderboard of 50 players in a scroll view: drag the list, flick it, scroll it with the
// mouse wheel or drag its scrollbar. The viewport is a mask, a layout group stacks the rows
// and sizes the content to them, and your own rank is pinned to the bottom while your row is
// out of view; tap the pin to scroll to it.
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
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/layoutgroup/layoutGroupComponent.h"
#include "framework/components/layoutgroup/layoutGroupComponentSystem.h"
#include "framework/components/layoutchild/layoutChildComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/components/scrollbar/scrollbarComponent.h"
#include "framework/components/scrollbar/scrollbarComponentSystem.h"
#include "framework/components/scrollview/scrollViewComponent.h"
#include "framework/components/scrollview/scrollViewComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/sprite.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

namespace
{
    const Color ORANGE(1.0f, 0.55f, 0.2f, 1.0f);
    const Color PANEL(0.16f, 0.18f, 0.23f, 1.0f);
    const Color ROW(0.21f, 0.24f, 0.3f, 1.0f);
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);
    const std::array<Color, 3> MEDALS{{Color(1.0f, 0.8f, 0.3f, 1.0f), Color(0.8f, 0.84f, 0.9f, 1.0f),
                                       Color(0.85f, 0.55f, 0.35f, 1.0f)}};

    constexpr int ME = 36;
    // Your row's top within the content: the padding, then 36 rows of 64 and their spacing
    constexpr float TOP = 10.0f + ME * 74.0f;

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

    /// A score with thousands separators, as `toLocaleString('en-US')` writes it.
    std::string withCommas(const int value)
    {
        std::string digits = std::to_string(value);
        for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
            digits.insert(static_cast<size_t>(i), ",");
        }
        return digits;
    }

    struct ElementProps
    {
        ElementType type = ElementType::Image;
        std::shared_ptr<Sprite> sprite;
        int spriteFrame = 0;
        Color color = LIGHT;
        Vector4 anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f);
        Vector2 pivot = Vector2(0.5f, 0.5f);
        std::optional<Vector4> margin;
        std::optional<float> width;
        std::optional<float> height;
        bool useInput = false;
        bool mask = false;
        FontResource* font = nullptr;
        std::string text;
        std::optional<int> fontSize;
    };
}

class ScrollViewExample final: public ExampleApp
{
public:
    ScrollViewExample()
        : ExampleApp({.title = "Scroll View", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ScreenComponentSystem>();
        options.registerComponentSystem<ElementComponentSystem>();
        options.registerComponentSystem<LayoutGroupComponentSystem>();
        options.registerComponentSystem<LayoutChildComponentSystem>();
        options.registerComponentSystem<ScrollbarComponentSystem>();
        options.registerComponentSystem<ScrollViewComponentSystem>();
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
        auto track = std::make_shared<Sprite>(atlas, std::vector<std::string>{"track"}, 4.0f, SpriteRenderMode::Sliced);
        _avatars = std::make_shared<Sprite>(atlas,
            std::vector<std::string>{"avatar-1", "avatar-2", "avatar-3", "avatar-4"});

        _title = createElement(screenEntity, {.type = ElementType::Text, .font = _bold, .text = "Leaderboard"});

        // The scroll view, on a panel, and its viewport: a mask that shows the content only inside
        // it. The viewport leaves room on the right for the scrollbar
        _scrollView = createElement(screenEntity, {.sprite = _panel, .color = PANEL, .width = 560.0f, .height = 540.0f});
        _viewport = createElement(_scrollView->entity(), {.sprite = _panel, .anchor = Vector4(0.0f, 0.0f, 1.0f, 1.0f),
            .margin = Vector4(8.0f, 8.0f, 32.0f, 8.0f), .mask = true});

        // The content hangs from the top edge of the viewport, as wide as the viewport. A layout
        // group stacks the rows in it, and each layout makes the content as tall as its rows and
        // padding
        _content = createElement(_viewport->entity(), {.type = ElementType::Group,
            .anchor = Vector4(0.0f, 1.0f, 1.0f, 1.0f), .pivot = Vector2(0.0f, 1.0f),
            .margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f), .useInput = true});
        auto* list = static_cast<LayoutGroupComponent*>(_content->entity()->addComponent<LayoutGroupComponent>());
        list->setOrientation(Orientation::Vertical);
        list->setSpacing(Vector2(0.0f, 10.0f));
        list->setPadding(Vector4(10.0f, 10.0f, 10.0f, 10.0f));
        list->setWidthFitting(LayoutFitting::Stretch);
        list->on("reflow", [this](const Vector4& bounds) { _content->setHeight(bounds.getW() + 20.0f); });

        // A scrollbar along the right edge, and its handle, which the scroll view sizes and moves
        ElementComponent* scrollbar = createElement(_scrollView->entity(), {.sprite = track, .color = ROW,
            .anchor = Vector4(1.0f, 0.0f, 1.0f, 1.0f), .pivot = Vector2(1.0f, 1.0f),
            .margin = Vector4(0.0f, 12.0f, 12.0f, 12.0f), .width = 12.0f});
        ElementComponent* handle = createElement(scrollbar->entity(), {.sprite = track, .color = MUTED,
            .anchor = Vector4(0.0f, 1.0f, 1.0f, 1.0f), .pivot = Vector2(1.0f, 1.0f),
            .margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f), .useInput = true});
        auto* bar = static_cast<ScrollbarComponent*>(scrollbar->entity()->addComponent<ScrollbarComponent>());
        bar->setOrientation(Orientation::Vertical);
        bar->setHandleEntity(handle->entity());

        _view = static_cast<ScrollViewComponent*>(_scrollView->entity()->addComponent<ScrollViewComponent>());
        _view->setHorizontal(false);
        _view->setVertical(true);
        _view->setScrollMode(ScrollMode::Bounce);
        _view->setBounceAmount(0.1f);
        _view->setFriction(0.05f);
        _view->setViewportEntity(_viewport->entity());
        _view->setContentEntity(_content->entity());
        _view->setVerticalScrollbarEntity(scrollbar->entity());

        // 50 players, with you in 37th place. Each tag is a unique pairing of the two lists of words
        const std::array<const char*, 10> first{
            {"Swift", "Silent", "Iron", "Lucky", "Crimson", "Frost", "Shadow", "Golden", "Wild", "Brave"}};
        const std::array<const char*, 10> second{
            {"Fox", "Raven", "Wolf", "Tiger", "Falcon", "Viper", "Bear", "Hawk", "Lynx", "Otter"}};
        std::vector<std::pair<std::string, int>> players;
        for (int i = 0; i < 50; ++i) {
            const std::string name = i == ME ? "You" : std::string(first[i % 10]) + second[(i * 3 + i / 10) % 10];
            players.emplace_back(name, 24800 - i * 410 - (i * 37) % 90);
        }
        for (int i = 0; i < 50; ++i) {
            createRow(_content->entity(), i + 1, players[i].first, players[i].second, {});
        }

        // Your rank, pinned to the bottom of the scroll view. It shows while your row is out of
        // view, which the scroll view's set:scroll event tells whenever the content moves
        _pin = createRow(_scrollView->entity(), ME + 1, "You", players[ME].second,
            {.anchor = Vector4(0.5f, 0.0f, 0.5f, 0.0f), .pivot = Vector2(0.5f, 0.0f), .useInput = true});
        _view->on("set:scroll", [this](const Vector2& scroll) {
            const float visible = _viewport->calculatedHeight();
            const float offset = scroll.y * (_content->calculatedHeight() - visible);
            _pin->entity()->setEnabled(TOP < offset || TOP + 64.0f > offset + visible);
        });

        // Tapping the pin scrolls your row into the middle of the view. The scroll position goes
        // from 0 at the top of the content to 1 at the bottom
        _pin->on("click", [this]() {
            const float visible = _viewport->calculatedHeight();
            const float y = (TOP + 32.0f - visible / 2.0f) / (_content->calculatedHeight() - visible);
            _view->setScroll(Vector2(0.0f, std::clamp(y, 0.0f, 1.0f)));
        });

        layout();
        return true;
    }

    void update(float /*dt*/) override { layout(); }

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
        element->setMask(props.mask);
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

    // A leaderboard row: rank, avatar, name and score
    ElementComponent* createRow(Entity* parent, const int rank, const std::string& name, const int score,
                                ElementProps properties)
    {
        const bool mine = name == "You";
        properties.sprite = _panel;
        properties.color = mine ? ORANGE : ROW;
        properties.height = 64.0f;
        ElementComponent* row = createElement(parent, properties);

        const auto text = [&](const std::string& value, const Vector4& anchor, const Vector2& pivot, FontResource* font,
                              const Color& color) {
            return createElement(row->entity(), {.type = ElementType::Text, .color = color, .anchor = anchor,
                .pivot = pivot, .font = font, .text = value, .fontSize = 24});
        };
        const Vector4 left(0.0f, 0.5f, 0.0f, 0.5f);
        const Vector2 leftPivot(0.0f, 0.5f);
        const Color textColor = mine ? PANEL : LIGHT;
        const Color rankColor = mine ? PANEL : (rank <= 3 ? MEDALS[rank - 1] : MUTED);
        text(std::to_string(rank), left, leftPivot, _bold, rankColor)->entity()->setLocalPosition(20.0f, 0.0f, 0.0f);
        createElement(row->entity(), {.sprite = _avatars, .spriteFrame = rank % 4, .anchor = left, .pivot = leftPivot,
                                      .width = 44.0f, .height = 44.0f})
            ->entity()->setLocalPosition(72.0f, 0.0f, 0.0f);
        text(name, left, leftPivot, _font, textColor)->entity()->setLocalPosition(132.0f, 0.0f, 0.0f);
        text(withCommas(score), Vector4(1.0f, 0.5f, 1.0f, 0.5f), Vector2(1.0f, 0.5f), _bold, textColor)
            ->entity()->setLocalPosition(-20.0f, 0.0f, 0.0f);
        return row;
    }

    // A wide list on landscape canvases, and a tall one on portrait canvases
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
        _scrollView->setWidth(portrait ? 500.0f : 560.0f);
        _scrollView->setHeight(portrait ? 760.0f : 540.0f);
        _scrollView->entity()->setLocalPosition(0.0f, portrait ? -40.0f : -30.0f, 0.0f);
        _title->entity()->setLocalPosition(0.0f, portrait ? 390.0f : 290.0f, 0.0f);

        // The pin is as wide as the rows, and centered on them
        _pin->setWidth(_scrollView->width() - 60.0f);
        _pin->entity()->setLocalPosition(-12.0f, 18.0f, 0.0f);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    FontResource* _bold = nullptr;
    std::shared_ptr<Sprite> _panel;
    std::shared_ptr<Sprite> _avatars;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _title = nullptr;
    ElementComponent* _scrollView = nullptr;
    ElementComponent* _viewport = nullptr;
    ElementComponent* _content = nullptr;
    ElementComponent* _pin = nullptr;
    ScrollViewComponent* _view = nullptr;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(ScrollViewExample)
