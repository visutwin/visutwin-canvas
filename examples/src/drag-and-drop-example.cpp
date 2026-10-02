// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// Port of upstream user-interface/drag-and-drop.
//
// Equipping gear by drag and drop. An ElementDragHelper makes each item follow the pointer, and
// when it is dropped, the item snaps into the slot it overlaps, if the slot takes it, or goes
// back to where it came from. The slots that take the item light up while it is dragged.
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
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/element/elementDragHelper.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/sprite.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

namespace
{
    const Color TAKES(0.55f, 0.32f, 0.15f, 1.0f);
    const Color PANEL(0.16f, 0.18f, 0.23f, 1.0f);
    const Color SLOT(0.22f, 0.25f, 0.31f, 1.0f);
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);

    /// The items, with their icons in the UI kit and the kind of gear slot that takes them (none:
    /// only the bag's slots).
    struct ItemData
    {
        const char* icon;
        Color color;
        const char* type;
    };

    const std::array<ItemData, 6> ITEMS{{
        {"icon-sword", Color(0.8f, 0.9f, 1.0f, 1.0f), "Weapon"},
        {"icon-shield", Color(0.8f, 0.6f, 0.4f, 1.0f), "Shield"},
        {"icon-gem", Color(1.0f, 0.3f, 0.45f, 1.0f), "Charm"},
        {"icon-potion", Color(1.0f, 0.35f, 0.4f, 1.0f), nullptr},
        {"icon-heart", Color(1.0f, 0.45f, 0.6f, 1.0f), "Charm"},
        {"icon-key", Color(1.0f, 0.8f, 0.3f, 1.0f), nullptr},
    }};
}

class DragAndDropExample final: public ExampleApp
{
public:
    DragAndDropExample()
        : ExampleApp({.title = "Drag and Drop", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        _font = _fontAsset->resourceAs<FontResource>();
        Texture* atlasTexture = _uiAtlasTexture->resourceAs<Texture>();
        if (!_font || !atlasTexture) {
            spdlog::error("Failed to load the Roboto font or ui/ui-atlas.png");
            return false;
        }

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));

        _screen = createScreen();
        Entity* screenEntity = _screen->entity();

        auto atlas = createUiAtlas(atlasTexture);
        auto panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        std::vector<std::string> iconFrames;
        for (const ItemData& item : ITEMS) {
            iconFrames.emplace_back(item.icon);
        }
        auto icons = std::make_shared<Sprite>(atlas, iconFrames);

        // The bag, with six slots that take any item, and the gear, with a slot for each kind of item
        _bag = createElement(screenEntity, {.sprite = panel, .color = PANEL, .width = 400.0f, .height = 310.0f});
        createElement(_bag->entity(), {.type = ElementType::Text, .text = "Bag", .fontSize = 30})
            ->entity()->setLocalPosition(0.0f, 120.0f, 0.0f);
        for (size_t i = 0; i < ITEMS.size(); ++i) {
            ElementComponent* slot = createElement(_bag->entity(), {.sprite = panel, .color = SLOT, .width = 104.0f,
                                                                    .height = 104.0f});
            slot->entity()->setName("slot " + std::to_string(i));
            slot->entity()->setLocalPosition((static_cast<float>(i % 3) - 1.0f) * 116.0f,
                                             40.0f - static_cast<float>(i / 3) * 116.0f, 0.0f);
            _bagSlots.push_back(slot);
        }

        _gear = createElement(screenEntity, {.sprite = panel, .color = PANEL, .width = 400.0f, .height = 310.0f});
        createElement(_gear->entity(), {.type = ElementType::Text, .text = "Equipped", .fontSize = 30})
            ->entity()->setLocalPosition(0.0f, 120.0f, 0.0f);
        const std::array<const char*, 3> types{{"Weapon", "Shield", "Charm"}};
        for (size_t i = 0; i < types.size(); ++i) {
            ElementComponent* slot = createElement(_gear->entity(), {.sprite = panel, .color = SLOT, .width = 104.0f,
                                                                     .height = 104.0f});
            slot->entity()->setName(types[i]);
            slot->entity()->setLocalPosition((static_cast<float>(i) - 1.0f) * 116.0f, 10.0f, 0.0f);
            createElement(slot->entity(), {.type = ElementType::Text, .color = MUTED, .text = types[i],
                                           .fontSize = 22})
                ->entity()->setLocalPosition(0.0f, -78.0f, 0.0f);
            _gearSlots.push_back(slot);
        }

        // The items are drawn over both panels, in a group of their own. Each is in a slot, where it
        // goes back to whenever it is not dropped in another one. The sword starts out equipped
        _layer = createElement(screenEntity, {.type = ElementType::Group, .anchor = Vector4(0.0f, 0.0f, 1.0f, 1.0f),
                                              .margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f)})->entity();
        for (size_t i = 0; i < ITEMS.size(); ++i) {
            auto item = std::make_unique<Item>();
            item->element = createElement(_layer, {.sprite = icons, .spriteFrame = static_cast<int>(i),
                .color = ITEMS[i].color, .width = 80.0f, .height = 80.0f, .useInput = true});
            item->type = ITEMS[i].type;
            item->slot = i == 0 ? _gearSlots[0] : _bagSlots[i];
            item->drag = std::make_unique<ElementDragHelper>(item->element);
            _items.push_back(std::move(item));
        }

        for (const auto& owned : _items) {
            Item* item = owned.get();
            // While an item is dragged, it is drawn over the other items, and the slots that take it
            // light up
            item->drag->on("drag:start", [this, item]() {
                _layer->addChild(item->element->entity());
                for (ElementComponent* slot : _gearSlots) {
                    slot->setColor(takes(slot, *item) ? TAKES : SLOT);
                }
            });

            // Dropped on a slot that takes it, the item moves in, and an item already there swaps to
            // the slot it came from, if that slot takes it. Anywhere else, it goes back
            item->drag->on("drag:end", [this, item]() {
                ElementComponent* target = nullptr;
                for (ElementComponent* slot : allSlots()) {
                    if (overlap(item->element, slot)) {
                        target = slot;
                        break;
                    }
                }
                Item* other = nullptr;
                for (const auto& o : _items) {
                    if (o.get() != item && o->slot == target) {
                        other = o.get();
                    }
                }
                if (target && takes(target, *item) && (!other || takes(item->slot, *other))) {
                    if (other) {
                        other->slot = item->slot;
                        place(*other);
                    }
                    item->slot = target;
                }
                place(*item);
                for (ElementComponent* slot : _gearSlots) {
                    slot->setColor(SLOT);
                }
            });
        }

        layout();
        return true;
    }

    void update(float /*dt*/) override { layout(); }

private:
    struct Item
    {
        ElementComponent* element = nullptr;
        const char* type = nullptr;
        ElementComponent* slot = nullptr;
        std::unique_ptr<ElementDragHelper> drag;
    };

    struct Bounds
    {
        float left, right, top, bottom;
    };

    /// An element with this example's defaults for what a call leaves unset.
    ElementComponent* createElement(Entity* parent, const ElementProps& props) const
    {
        return visutwin::canvas::createElement(engine(), parent, props, {.type = ElementType::Image,
            .color = LIGHT, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .pivot = Vector2(0.5f, 0.5f),
            .font = _font});
    }

    std::vector<ElementComponent*> allSlots() const
    {
        std::vector<ElementComponent*> slots = _gearSlots;
        slots.insert(slots.end(), _bagSlots.begin(), _bagSlots.end());
        return slots;
    }

    void place(const Item& item) const { item.element->entity()->setPosition(item.slot->entity()->position()); }

    bool takes(const ElementComponent* slot, const Item& item) const
    {
        return std::find(_bagSlots.begin(), _bagSlots.end(), slot) != _bagSlots.end() ||
            (item.type && slot->entity()->name() == item.type);
    }

    // The bounds of an element, in canvas points from the top-left of the canvas
    static Bounds bounds(ElementComponent* element)
    {
        const auto& corners = element->canvasCorners();
        return {corners[0].x, corners[2].x, corners[2].y, corners[0].y};
    }

    static bool overlap(ElementComponent* a, ElementComponent* b)
    {
        const Bounds p = bounds(a);
        const Bounds q = bounds(b);
        return p.left < q.right && p.right > q.left && p.top < q.bottom && p.bottom > q.top;
    }

    // The panels side by side on landscape canvases, and stacked on portrait ones. The items follow
    // their slots
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
        _bag->entity()->setLocalPosition(portrait ? 0.0f : -220.0f, portrait ? 190.0f : 0.0f, 0.0f);
        _gear->entity()->setLocalPosition(portrait ? 0.0f : 220.0f, portrait ? -170.0f : 0.0f, 0.0f);
        for (const auto& item : _items) {
            place(*item);
        }
    }

    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _bag = nullptr;
    ElementComponent* _gear = nullptr;
    Entity* _layer = nullptr;
    std::vector<ElementComponent*> _bagSlots;
    std::vector<ElementComponent*> _gearSlots;
    std::vector<std::unique_ptr<Item>> _items;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(DragAndDropExample)
