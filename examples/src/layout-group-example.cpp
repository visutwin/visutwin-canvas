// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// Port of upstream user-interface/layout-group.
//
// A backpack screen built from three layout groups: the item's details are a vertical list,
// the bag is a grid whose last row is centered, and the actions are a toolbar that shares its
// width. Tap an item, then loot, sort or drop, to try them.
//
#include <algorithm>
#include <array>
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
#include "framework/components/layoutchild/layoutChildComponent.h"
#include "framework/components/layoutchild/layoutChildComponentSystem.h"
#include "framework/components/layoutgroup/layoutGroupComponent.h"
#include "framework/components/layoutgroup/layoutGroupComponentSystem.h"
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
    const Color SLOT(0.22f, 0.25f, 0.31f, 1.0f);
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);

    /// The items that can be in the bag, each with an icon in the UI kit and its details.
    struct Item
    {
        const char* kind;
        const char* name;
        Color color;
        std::vector<std::string> stats;
    };

    const std::array<Item, 7> ITEMS{{
        {"sword", "Iron Sword", Color(0.8f, 0.9f, 1.0f, 1.0f), {"+12 attack", "Weighs 3 kg", "Sells for 40 gold"}},
        {"potion", "Health Potion", Color(1.0f, 0.35f, 0.4f, 1.0f), {"Heals 50 health", "Sells for 15 gold"}},
        {"shield", "Oak Shield", Color(0.8f, 0.6f, 0.4f, 1.0f), {"+8 defense", "Weighs 5 kg", "Sells for 30 gold"}},
        {"gem", "Ruby", Color(1.0f, 0.3f, 0.45f, 1.0f), {"Sells for 120 gold"}},
        {"key", "Crypt Key", Color(1.0f, 0.8f, 0.3f, 1.0f), {"Opens the crypt"}},
        {"flame", "Fire Scroll", Color(1.0f, 0.55f, 0.2f, 1.0f), {"Deals 30 damage", "One use", "Sells for 60 gold"}},
        {"heart", "Heart Crystal", Color(1.0f, 0.45f, 0.6f, 1.0f), {"+20 health", "Sells for 80 gold"}},
    }};

    const Item& itemOf(const std::string& kind)
    {
        for (const Item& item : ITEMS) {
            if (kind == item.kind) {
                return item;
            }
        }
        return ITEMS[0];
    }
}

class LayoutGroupExample final: public ExampleApp
{
public:
    LayoutGroupExample()
        : ExampleApp({.title = "Layout Group", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options, {.button = true});
        options.registerComponentSystem<LayoutGroupComponentSystem>();
        options.registerComponentSystem<LayoutChildComponentSystem>();
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
        _screenEntity = _screen->entity();

        auto atlas = createUiAtlas(atlasTexture);
        _panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        _outline = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel-outline"}, 2.0f,
                                            SpriteRenderMode::Sliced);
        std::vector<std::string> iconFrames;
        for (const Item& item : ITEMS) {
            iconFrames.push_back(std::string("icon-") + item.kind);
        }
        _icons = std::make_shared<Sprite>(atlas, iconFrames);

        // The details of the chosen item: a vertical list, on its panel, that stretches each row
        // to the width of the panel, less its padding
        _details = createElement(_screenEntity, {.sprite = _panel, .color = PANEL, .width = 300.0f, .height = 400.0f});
        auto* detailsLayout = static_cast<LayoutGroupComponent*>(
            _details->entity()->addComponent<LayoutGroupComponent>());
        detailsLayout->setOrientation(Orientation::Vertical);
        detailsLayout->setAlignment(Vector2(0.0f, 1.0f));
        detailsLayout->setPadding(Vector4(10.0f, 10.0f, 10.0f, 10.0f));
        detailsLayout->setSpacing(Vector2(0.0f, 10.0f));
        detailsLayout->setWidthFitting(LayoutFitting::Stretch);
        detailsLayout->setHeightFitting(LayoutFitting::None);
        detailsLayout->setWrap(false);

        // A row for the name, and one for each line of the item with the most details. The list
        // lays out only the rows that are enabled
        for (int i = 0; i < 4; ++i) {
            ElementComponent* row = createElement(_details->entity(), {.sprite = _panel, .color = SLOT, .height = 60.0f});
            ElementComponent* text = createElement(row->entity(), {.type = ElementType::Text,
                .color = i == 0 ? ORANGE : LIGHT, .anchor = Vector4(0.0f, 0.5f, 0.0f, 0.5f),
                .pivot = Vector2(0.0f, 0.5f), .font = i == 0 ? _bold : _font, .fontSize = 24});
            text->entity()->setLocalPosition(16.0f, 0.0f, 0.0f);
            _rows.push_back(row);
            _rowTexts.push_back(text);
        }

        // The bag: a grid of 100 x 100 slots in a group 320 units wide, so three fit on each row,
        // and an alignment that centers each row, including a last row that is not full
        _bag = createElement(_screenEntity, {.sprite = _panel, .color = PANEL, .width = 360.0f, .height = 400.0f});
        createElement(_bag->entity(), {.type = ElementType::Text, .font = _bold, .text = "Backpack"})
            ->entity()->setLocalPosition(0.0f, 164.0f, 0.0f);
        ElementComponent* grid = createElement(_bag->entity(), {.type = ElementType::Group, .width = 320.0f,
                                                                .height = 320.0f});
        _grid = grid->entity();
        _grid->setLocalPosition(0.0f, -30.0f, 0.0f);
        auto* gridLayout = static_cast<LayoutGroupComponent*>(_grid->addComponent<LayoutGroupComponent>());
        gridLayout->setOrientation(Orientation::Horizontal);
        gridLayout->setAlignment(Vector2(0.5f, 1.0f));
        gridLayout->setPadding(Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        gridLayout->setSpacing(Vector2(10.0f, 10.0f));
        gridLayout->setWidthFitting(LayoutFitting::None);
        gridLayout->setHeightFitting(LayoutFitting::None);
        gridLayout->setWrap(true);

        // The toolbar: a row, on its panel, that shares the width of the panel between its buttons
        // and gives them its height, less its padding
        _toolbar = createElement(_screenEntity, {.sprite = _panel, .color = PANEL, .width = 690.0f, .height = 80.0f});
        auto* toolbarLayout = static_cast<LayoutGroupComponent*>(
            _toolbar->entity()->addComponent<LayoutGroupComponent>());
        toolbarLayout->setOrientation(Orientation::Horizontal);
        toolbarLayout->setAlignment(Vector2(0.0f, 0.5f));
        toolbarLayout->setPadding(Vector4(10.0f, 10.0f, 10.0f, 10.0f));
        toolbarLayout->setSpacing(Vector2(10.0f, 0.0f));
        toolbarLayout->setWidthFitting(LayoutFitting::Stretch);
        toolbarLayout->setHeightFitting(LayoutFitting::Stretch);
        toolbarLayout->setWrap(false);

        _loot = createButton("Loot", ORANGE);
        ButtonComponent* sort = createButton("Sort", Color(0.26f, 0.29f, 0.36f, 1.0f));
        _drop = createButton("Drop", Color(0.75f, 0.25f, 0.22f, 1.0f));

        // A maximum width stops the drop button from growing as wide as the others
        auto* dropChild = static_cast<LayoutChildComponent*>(_drop->entity()->addComponent<LayoutChildComponent>());
        dropChild->setMaxWidth(120.0f);

        // The bag starts with the first five kinds of item
        for (size_t i = 0; i < 5; ++i) {
            add(ITEMS[i].kind);
        }
        choose(dynamic_cast<Entity*>(_grid->children().front().get()));

        // Loot adds the next kind of item. Sort adds the slots again in the order of their names,
        // which is the order the grid follows. Drop removes the chosen item
        _looted = _grid->children().size();
        _loot->on("click", [this]() { choose(add(ITEMS[_looted++ % ITEMS.size()].kind)); });
        sort->on("click", [this]() {
            std::vector<Entity*> sorted;
            for (const auto& child : _grid->children()) {
                sorted.push_back(static_cast<Entity*>(child.get()));
            }
            std::stable_sort(sorted.begin(), sorted.end(), [](const Entity* a, const Entity* b) {
                return std::string(itemOf(a->name()).name) < std::string(itemOf(b->name()).name);
            });
            for (Entity* slot : sorted) {
                _grid->addChild(slot);
            }
        });
        _drop->on("click", [this]() {
            Entity* slot = _chosen;
            choose(nullptr);
            // Upstream's destroy also takes the entity out of its parent; here the parent owns it
            slot->destroy();
            (void)_grid->removeChild(slot);
            _loot->setActive(true);
        });

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
            .font = _font});
    }

    // A toolbar button. It starts 100 units wide, and the toolbar stretches it
    ButtonComponent* createButton(const std::string& text, const Color& color)
    {
        ElementComponent* element = createElement(_toolbar->entity(), {.sprite = _panel, .color = color,
            .width = 100.0f, .height = 60.0f, .useInput = true});
        auto* button = static_cast<ButtonComponent*>(element->entity()->addComponent<ButtonComponent>());
        button->setImageEntity(element->entity());
        button->setHoverTint(Color(color.r * 0.8f + 0.2f, color.g * 0.8f + 0.2f, color.b * 0.8f + 0.2f, 1.0f));
        button->setPressedTint(Color(color.r * 0.7f, color.g * 0.7f, color.b * 0.7f, 1.0f));
        button->setInactiveTint(Color(0.3f, 0.32f, 0.37f, 1.0f));
        createElement(element->entity(), {.type = ElementType::Text, .font = _bold, .text = text, .fontSize = 26});
        return button;
    }

    // Show the chosen item's name and details, a row each. The drop button shows only while
    // there is an item to drop, and the toolbar shares its width between the buttons that are
    // enabled
    void choose(Entity* slot)
    {
        if (_chosen) {
            _chosen->findByName("ring")->setEnabled(false);
        }
        _chosen = slot;
        std::vector<std::string> lines{"Tap an item"};
        if (slot) {
            const Item& item = itemOf(slot->name());
            lines = {item.name};
            lines.insert(lines.end(), item.stats.begin(), item.stats.end());
        }
        for (size_t i = 0; i < _rows.size(); ++i) {
            _rows[i]->entity()->setEnabled(i < lines.size());
            _rowTexts[i]->setText(i < lines.size() ? lines[i] : "");
        }
        if (slot) {
            slot->findByName("ring")->setEnabled(true);
        }
        _drop->entity()->setEnabled(slot != nullptr);
    }

    // Add an item to the bag. The grid places its slot after the others
    Entity* add(const std::string& kind)
    {
        ElementComponent* slot = createElement(_grid, {.sprite = _panel, .color = SLOT, .width = 100.0f,
                                                       .height = 100.0f, .useInput = true});
        Entity* entity = slot->entity();
        entity->setName(kind);
        const auto index = static_cast<int>(std::find_if(ITEMS.begin(), ITEMS.end(), [&](const Item& item) {
            return kind == item.kind;
        }) - ITEMS.begin());
        createElement(entity, {.sprite = _icons, .spriteFrame = index, .color = itemOf(kind).color, .width = 64.0f,
                               .height = 64.0f});
        ElementComponent* ring = createElement(entity, {.sprite = _outline, .color = ORANGE, .width = 100.0f,
                                                        .height = 100.0f});
        ring->entity()->setName("ring");
        ring->entity()->setEnabled(false);
        slot->on("click", [this, entity]() { choose(entity); });
        _loot->setActive(_grid->children().size() < 9);
        return entity;
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
        _details->setWidth(portrait ? 360.0f : 300.0f);
        _details->setHeight(portrait ? 290.0f : 400.0f);
        _details->entity()->setLocalPosition(portrait ? 0.0f : -195.0f, portrait ? 260.0f : 50.0f, 0.0f);
        _bag->entity()->setLocalPosition(portrait ? 0.0f : 165.0f, portrait ? -105.0f : 50.0f, 0.0f);
        _toolbar->setWidth(portrait ? 360.0f : 690.0f);
        _toolbar->entity()->setLocalPosition(0.0f, portrait ? -365.0f : -210.0f, 0.0f);
    }

    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    FontResource* _bold = nullptr;
    std::shared_ptr<Sprite> _panel;
    std::shared_ptr<Sprite> _outline;
    std::shared_ptr<Sprite> _icons;
    Entity* _screenEntity = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _details = nullptr;
    ElementComponent* _bag = nullptr;
    ElementComponent* _toolbar = nullptr;
    Entity* _grid = nullptr;
    std::vector<ElementComponent*> _rows;
    std::vector<ElementComponent*> _rowTexts;
    ButtonComponent* _loot = nullptr;
    ButtonComponent* _drop = nullptr;
    Entity* _chosen = nullptr;
    size_t _looted = 0;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(LayoutGroupExample)
