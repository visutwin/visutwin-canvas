// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 29.09.2026
//
// Port of upstream user-interface/input-events.
//
// A map that drops a marker wherever the ground is pressed, under a HUD. The HUD is a group
// element filling the screen with input off; its toolbar holds three tools (Pin, Treasure,
// Danger), each an image with an icon. Hovering a tool names it above the toolbar, and ONE
// click listener on the toolbar handles all three tools, because element events bubble up
// from the element that was hit, which the event names. The game reads the mouse and touch
// DEVICES, as game code does, so without help a press on the HUD drops a marker behind it
// too. Tick "Keep HUD taps from the game" and the HUD's mousedown / touchstart handler stops
// those presses propagating, which keeps them from the devices as well.
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
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "platform/input/mouse.h"
#include "platform/input/touchDevice.h"
#include "scene/materials/standardMaterial.h"
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

    // The tools, each with an icon in the UI kit and the color of its markers
    struct Tool
    {
        const char* name;
        Color color;
    };
    const std::array<Tool, 3> TOOLS{{
        {"Pin", Color(1.0f, 0.55f, 0.2f, 1.0f)},
        {"Treasure", Color(1.0f, 0.8f, 0.3f, 1.0f)},
        {"Danger", Color(0.95f, 0.3f, 0.25f, 1.0f)},
    }};
}

class InputEventsExample final: public ExampleApp
{
public:
    InputEventsExample()
        : ExampleApp({.title = "Input Events", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        _font = _fontAsset->resourceAs<FontResource>();
        Texture* atlasTexture = _uiAtlasTexture->resourceAs<Texture>();
        if (!_font || !atlasTexture) {
            spdlog::error("Failed to load fonts/roboto-bold.json or ui/ui-atlas.png");
            return false;
        }

        // A material of one color, for the ground and the markers
        for (const Tool& tool : TOOLS) {
            _materials.push_back(createMaterial(tool.color));
        }

        // The map: the ground, a light, and a camera looking down at it
        scene()->setAmbientLight(0.35f, 0.37f, 0.42f);
        _groundMaterial = createMaterial(Color(0.3f, 0.45f, 0.35f, 1.0f));
        createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f), Vector3(200.0f, 1.0f, 200.0f));

        auto* light = createDirectionalLight(Vector3(50.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowDistance(30.0f);
        }

        auto* cameraEntity = createCamera(Vector3(0.0f, 12.0f, 9.0f));
        cameraEntity->lookAt(0.0f, 0.0f, 0.0f);
        _camera = cameraEntity->findComponent<CameraComponent>();
        _camera->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));

        // The HUD's screen
        _screen = createScreen();
        Entity* screenEntity = _screen->entity();

        auto atlas = std::make_shared<TextureAtlas>();
        atlas->setTexture(atlasTexture);
        const Vector2 centre(0.5f, 0.5f);
        const Vector4 none(0.0f, 0.0f, 0.0f, 0.0f);
        atlas->setFrame("panel", {.rect = Vector4(548.0f, 660.0f, 128.0f, 128.0f), .pivot = centre,
                                  .border = Vector4(32.0f, 32.0f, 32.0f, 32.0f)});
        atlas->setFrame("icon-pin", {.rect = Vector4(4.0f, 348.0f, 96.0f, 96.0f), .pivot = centre, .border = none});
        atlas->setFrame("icon-star", {.rect = Vector4(524.0f, 348.0f, 96.0f, 96.0f), .pivot = centre, .border = none});
        atlas->setFrame("icon-flame", {.rect = Vector4(628.0f, 556.0f, 96.0f, 96.0f), .pivot = centre, .border = none});
        atlas->setFrame("checkbox", {.rect = Vector4(4.0f, 276.0f, 64.0f, 64.0f), .pivot = centre, .border = none});
        atlas->setFrame("check", {.rect = Vector4(940.0f, 380.0f, 64.0f, 64.0f), .pivot = centre, .border = none});
        _panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        _icons = std::make_shared<Sprite>(
            atlas, std::vector<std::string>{"icon-pin", "icon-star", "icon-flame", "checkbox", "check"});

        // The HUD is a group element that fills the screen, with input off, so that only its
        // panels stop presses from reaching the game. The toolbar is one of them, at the
        // bottom of the screen
        _hud = createElement(screenEntity, {.type = ElementType::Group, .anchor = Vector4(0.0f, 0.0f, 1.0f, 1.0f),
                                            .margin = none});
        ElementComponent* toolbar = createElement(_hud->entity(), {
            .sprite = _panel, .color = PANEL, .anchor = Vector4(0.5f, 0.0f, 0.5f, 0.0f), .pivot = Vector2(0.5f, 0.0f),
            .width = 296.0f, .height = 104.0f, .useInput = true});
        toolbar->entity()->setLocalPosition(0.0f, 24.0f, 0.0f);
        for (size_t i = 0; i < TOOLS.size(); ++i) {
            ElementComponent* button = createElement(toolbar->entity(), {.sprite = _panel, .color = PANEL,
                                                                        .width = 80.0f, .height = 80.0f,
                                                                        .useInput = true});
            button->entity()->setLocalPosition((static_cast<float>(i) - 1.0f) * 92.0f, 0.0f, 0.0f);
            createElement(button->entity(), {.sprite = _icons, .spriteFrame = static_cast<int>(i),
                                             .color = TOOLS[i].color, .width = 48.0f, .height = 48.0f});
            _buttons.push_back(button);
        }

        // The name of the tool under the pointer, above the toolbar. Hovering is a mouse
        // extra: a tap chooses the tool, and shows its name too
        _label = createElement(_hud->entity(), {.type = ElementType::Text, .anchor = Vector4(0.5f, 0.0f, 0.5f, 0.0f),
                                                .pivot = Vector2(0.5f, 0.0f), .fontSize = 26});
        _label->entity()->setLocalPosition(0.0f, 140.0f, 0.0f);
        for (size_t i = 0; i < _buttons.size(); ++i) {
            _buttons[i]->on("mouseenter", [this, i]() { show(i); });
            _buttons[i]->on("mouseleave", [this]() { show(_tool); });
        }

        // One listener on the toolbar handles the clicks of all its buttons: events bubble up
        // from the element that was hit, which the event names
        toolbar->on("click", [this](ElementInputEvent* event) {
            const auto it = std::find(_buttons.begin(), _buttons.end(), event->element);
            if (it != _buttons.end()) {
                choose(static_cast<size_t>(it - _buttons.begin()));
            }
        });
        choose(0);

        // Stop presses on the HUD from propagating, which keeps them from the mouse and touch
        // devices too. The checkbox turns it on and off, to compare
        const auto block = [this](ElementInputEvent* event) {
            if (_blocking) {
                event->stopPropagation();
            }
        };
        _hud->on("mousedown", block);
        _hud->on("touchstart", block);

        ElementComponent* option = createElement(_hud->entity(), {
            .sprite = _panel, .color = PANEL, .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f), .pivot = Vector2(0.0f, 1.0f),
            .width = 400.0f, .height = 64.0f, .useInput = true});
        option->entity()->setLocalPosition(24.0f, -24.0f, 0.0f);
        ElementComponent* box = createElement(option->entity(), {.sprite = _icons, .spriteFrame = 3, .color = MUTED,
                                                                 .width = 36.0f, .height = 36.0f});
        box->entity()->setLocalPosition(-166.0f, 0.0f, 0.0f);
        _tick = createElement(box->entity(), {.sprite = _icons, .spriteFrame = 4, .width = 36.0f, .height = 36.0f});
        createElement(option->entity(), {.type = ElementType::Text, .text = "Keep HUD taps from the game",
                                         .fontSize = 24})
            ->entity()->setLocalPosition(20.0f, 0.0f, 0.0f);
        option->on("click", [this]() {
            _blocking = !_blocking;
            _tick->entity()->setEnabled(_blocking);
        });
        _tick->entity()->setEnabled(_blocking);

        // The number of markers the game has dropped, which a leaking tap on the HUD adds to
        _count = createElement(_hud->entity(), {.type = ElementType::Text, .anchor = Vector4(1.0f, 1.0f, 1.0f, 1.0f),
                                                .pivot = Vector2(1.0f, 1.0f), .text = "Markers dropped: 0",
                                                .fontSize = 26});

        // The game's input: a press on the ground drops a marker of the current tool there. It
        // reads the mouse and touch devices, as game code does, rather than element events
        if (Mouse* mouse = engine()->mouse()) {
            mouse->on("mousedown", [this](const MouseEvent& event) {
                // A tap is followed by emulated mouse events, which would drop a second
                // marker; they carry fromTouch
                if (!event.fromTouch) {
                    drop(event.x, event.y);
                }
            });
        }
        if (TouchDevice* touch = engine()->touch()) {
            touch->on("touchstart", [this](const TouchEvent& event) {
                if (event.changed.empty()) {
                    return;
                }
                // Touches are in canvas points, as mouse events are
                drop(event.changed[0].x, event.changed[0].y);
            });
        }

        // A few markers are on the map already
        addMarker(-3.5f, -2.0f, 0);
        addMarker(2.5f, 0.5f, 1);
        addMarker(4.0f, -3.0f, 2);

        layout();
        return true;
    }

    void update(float /*dt*/) override
    {
        layout();
    }

private:
    std::shared_ptr<StandardMaterial> createMaterial(const Color& color) const
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        return material;
    }

    /// An element with this example's defaults for what a call leaves unset.
    ElementComponent* createElement(Entity* parent, const ElementProps& props) const
    {
        return visutwin::canvas::createElement(engine(), parent, props, {.type = ElementType::Image,
            .color = LIGHT, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .pivot = Vector2(0.5f, 0.5f),
            .font = _font});
    }

    void show(const size_t i) { _label->setText(std::string(TOOLS[i].name) + " marker"); }

    void choose(const size_t i)
    {
        _tool = i;
        for (size_t j = 0; j < _buttons.size(); ++j) {
            _buttons[j]->setColor(j == i ? Color(0.3f, 0.34f, 0.42f, 1.0f) : PANEL);
        }
        show(i);
    }

    void addMarker(const float x, const float z, const size_t kind)
    {
        Entity* marker = createPrimitive("cone", _materials[kind].get(), Vector3(x, 0.35f, z),
                                         Vector3(0.5f, 0.7f, 0.5f));
        _markers.push_back(marker);
        if (_markers.size() > 12) {
            Entity* oldest = _markers.front();
            _markers.pop_front();
            oldest->destroy();
            auto removed = oldest->remove();
        }
        _count->setText("Markers dropped: " + std::to_string(++_dropped));
    }

    // Find where a press meets the ground, from points on the camera's near and far planes
    void drop(const float x, const float y)
    {
        const Vector3 nearPoint = _camera->screenToWorld(x, y, _camera->camera()->nearClip());
        const Vector3 farPoint = _camera->screenToWorld(x, y, _camera->camera()->farClip());
        const float t = nearPoint.getY() / (nearPoint.getY() - farPoint.getY());
        addMarker(nearPoint.getX() + (farPoint.getX() - nearPoint.getX()) * t,
                  nearPoint.getZ() + (farPoint.getZ() - nearPoint.getZ()) * t, _tool);
    }

    // On portrait canvases, use a portrait reference resolution, fit the map to the width of
    // the view, and move the count below the option
    void layout()
    {
        const auto [w, h] = engine()->canvasSize();
        if (w == _laidOutWidth && h == _laidOutHeight) {
            return;
        }
        _laidOutWidth = w;
        _laidOutHeight = h;
        const bool portrait = h > w;
        _screen->setReferenceResolution(portrait ? Vector2(540.0f, 960.0f) : Vector2(1280.0f, 720.0f));
        _camera->camera()->setHorizontalFov(portrait);
        _count->entity()->setLocalPosition(-24.0f, portrait ? -108.0f : -40.0f, 0.0f);
    }

    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    std::shared_ptr<Sprite> _panel;
    std::shared_ptr<Sprite> _icons;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<StandardMaterial> _groundMaterial;
    CameraComponent* _camera = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _hud = nullptr;
    ElementComponent* _label = nullptr;
    ElementComponent* _tick = nullptr;
    ElementComponent* _count = nullptr;
    std::vector<ElementComponent*> _buttons;
    std::deque<Entity*> _markers;
    size_t _tool = 0;
    int _dropped = 0;
    bool _blocking = false;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(InputEventsExample)
