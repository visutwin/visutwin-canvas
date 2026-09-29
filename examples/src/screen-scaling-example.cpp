// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/screen-scaling.
//
// A mobile game's HUD designed at a 1280 x 720 reference resolution: health and coins along
// the top, a joystick and a minimap along the bottom, each 24 units in from its corner, and
// an outline showing the reference area. Buttons on a second screen, drawn over the HUD's
// with a higher priority and always scaled with Blend, change how the HUD's screen scales:
// Blend fits the reference to the canvas where None keeps its units in pixels, and Scale
// Blend picks the axis to follow (Fit follows whichever has less room).
//
// DEVIATIONS:
// - no "Pixel ratio" button: the drawable here always follows the display's density, and the
//   engine has no maximum pixel ratio yet; the other two buttons are centred as a pair.
// - the readout's canvas size is the drawable's pixels, which is upstream's `device.width` at
//   the device's pixel ratio.
//
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
    const Color PANEL(0.16f, 0.18f, 0.23f, 1.0f);
    const Color LIGHT(0.95f, 0.96f, 0.98f, 1.0f);
    const Color MUTED(0.6f, 0.64f, 0.72f, 1.0f);

    const Vector4 CENTRE_ANCHOR(0.5f, 0.5f, 0.5f, 0.5f);
    const Vector2 CENTRE_PIVOT(0.5f, 0.5f);

    /// Upstream's element properties that this example sets.
    struct ElementProps
    {
        ElementType type = ElementType::Image;
        std::shared_ptr<Sprite> sprite;
        int spriteFrame = 0;
        Color color = LIGHT;
        Vector4 anchor = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        Vector2 pivot = Vector2(0.0f, 0.0f);
        std::optional<float> width;
        std::optional<float> height;
        bool useInput = false;
        std::string text;
        int fontSize = 32;
    };

    /// A toggle's label and what it applies.
    struct Choice
    {
        std::string label;
        std::function<void()> apply;
    };
}

class ScreenScalingExample final: public ExampleApp
{
public:
    ScreenScalingExample()
        : ExampleApp({.title = "Screen Scaling", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

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
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        if (const auto res = _fontAsset->resource(); res && std::holds_alternative<FontResource*>(*res)) {
            _font = std::get<FontResource*>(*res);
        }
        Texture* atlasTexture = nullptr;
        if (const auto res = _uiAtlasTexture->resource(); res && std::holds_alternative<Texture*>(*res)) {
            atlasTexture = std::get<Texture*>(*res);
        }
        if (!_font || !atlasTexture) {
            spdlog::error("Failed to load fonts/roboto-bold.json or ui/ui-atlas.png");
            return false;
        }

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.2f, 0.3f, 0.26f, 1.0f));

        // Frames of upstream's UI kit atlas (ui-atlas.mjs)
        auto atlas = std::make_shared<TextureAtlas>();
        atlas->setTexture(atlasTexture);
        const Vector4 border32(32.0f, 32.0f, 32.0f, 32.0f);
        const Vector4 none(0.0f, 0.0f, 0.0f, 0.0f);
        atlas->setFrame("panel", {.rect = Vector4(548.0f, 660.0f, 128.0f, 128.0f), .pivot = CENTRE_PIVOT,
                                  .border = border32});
        atlas->setFrame("panel-outline", {.rect = Vector4(684.0f, 660.0f, 128.0f, 128.0f), .pivot = CENTRE_PIVOT,
                                          .border = border32});
        atlas->setFrame("circle", {.rect = Vector4(412.0f, 660.0f, 128.0f, 128.0f), .pivot = CENTRE_PIVOT,
                                   .border = none});
        atlas->setFrame("knob", {.rect = Vector4(76.0f, 276.0f, 64.0f, 64.0f), .pivot = CENTRE_PIVOT, .border = none});
        const auto sliced = [&atlas](const std::string& frame) {
            return std::make_shared<Sprite>(atlas, std::vector<std::string>{frame}, 2.0f, SpriteRenderMode::Sliced);
        };
        auto panel = sliced("panel");
        _panelSprite = panel;
        auto outline = sliced("panel-outline");
        auto icons = std::make_shared<Sprite>(atlas, std::vector<std::string>{"circle", "knob"});

        // The HUD's screen, designed at 1280 x 720, and the reference area, centered on it
        auto* hudEntity = new Entity();
        hudEntity->setEngine(engine());
        _hud = static_cast<ScreenComponent*>(hudEntity->addComponent<ScreenComponent>());
        _hud->setScreenSpace(true);
        _hud->setScaleMode(ScreenScaleMode::Blend);
        _hud->setReferenceResolution(Vector2(1280.0f, 720.0f));
        _hud->setScaleBlend(0.5f);
        root()->addChild(hudEntity);
        ElementComponent* reference = createElement(hudEntity, {.sprite = outline, .color = MUTED,
            .anchor = CENTRE_ANCHOR, .pivot = CENTRE_PIVOT, .width = 1280.0f, .height = 720.0f});
        createElement(reference->entity(), {.type = ElementType::Text, .color = MUTED,
            .anchor = Vector4(0.5f, 1.0f, 0.5f, 1.0f), .pivot = Vector2(0.5f, 1.0f),
            .text = "Reference area, 1280 × 720", .fontSize = 22})
            ->entity()->setLocalPosition(0.0f, -96.0f, 0.0f);

        // Health and coins along the top, a joystick and a minimap along the bottom
        const auto corner = [&](const float x, const float y, ElementProps props) {
            props.anchor = Vector4(x, y, x, y);
            props.pivot = Vector2(x, y);
            ElementComponent* element = createElement(hudEntity, props);
            element->entity()->setLocalPosition(x > 0.0f ? -24.0f : 24.0f, y > 0.0f ? -24.0f : 24.0f, 0.0f);
            return element;
        };
        ElementComponent* health = corner(0.0f, 1.0f, {.sprite = panel, .color = PANEL, .width = 320.0f, .height = 56.0f});
        createElement(health->entity(), {.sprite = panel, .color = Color(1.0f, 0.35f, 0.4f, 1.0f),
            .anchor = CENTRE_ANCHOR, .pivot = CENTRE_PIVOT, .width = 288.0f, .height = 24.0f});
        ElementComponent* coins = corner(1.0f, 1.0f, {.sprite = panel, .color = PANEL, .width = 200.0f, .height = 56.0f});
        createElement(coins->entity(), {.type = ElementType::Text, .anchor = CENTRE_ANCHOR, .pivot = CENTRE_PIVOT,
            .text = "1,250 coins", .fontSize = 28});
        ElementComponent* stick = corner(0.0f, 0.0f, {.sprite = icons, .spriteFrame = 0, .color = PANEL,
            .width = 200.0f, .height = 200.0f});
        createElement(stick->entity(), {.sprite = icons, .spriteFrame = 1, .anchor = CENTRE_ANCHOR,
            .pivot = CENTRE_PIVOT, .width = 80.0f, .height = 80.0f});
        ElementComponent* map = corner(1.0f, 0.0f, {.sprite = panel, .color = Color(0.3f, 0.45f, 0.35f, 1.0f),
            .width = 200.0f, .height = 200.0f});
        createElement(map->entity(), {.sprite = icons, .spriteFrame = 0, .color = ORANGE, .anchor = CENTRE_ANCHOR,
            .pivot = CENTRE_PIVOT, .width = 20.0f, .height = 20.0f});

        // The controls are on a second screen, drawn over the HUD's with a higher priority and
        // always scaled with Blend, so they stay usable whatever the HUD's screen does
        auto* controlsEntity = new Entity();
        controlsEntity->setEngine(engine());
        _controls = static_cast<ScreenComponent*>(controlsEntity->addComponent<ScreenComponent>());
        _controls->setScreenSpace(true);
        _controls->setScaleMode(ScreenScaleMode::Blend);
        _controls->setReferenceResolution(Vector2(1280.0f, 720.0f));
        _controls->setPriority(10);
        root()->addChild(controlsEntity);
        _readout = createElement(controlsEntity, {.type = ElementType::Text, .anchor = CENTRE_ANCHOR,
            .pivot = CENTRE_PIVOT, .fontSize = 24});

        // Fit follows whichever axis has less room, so the whole reference area stays on screen
        _toggles.push_back(createToggle(controlsEntity, "Scale mode", {
            {"Blend", [this] { _hud->setScaleMode(ScreenScaleMode::Blend); }},
            {"None", [this] { _hud->setScaleMode(ScreenScaleMode::None); }},
        }));
        _toggles.push_back(createToggle(controlsEntity, "Scale blend", {
            {"Fit", [this] { _blend.reset(); fitReference(); }},
            {"0 (width)", [this] { _blend = 0.0f; fitReference(); }},
            {"0.5", [this] { _blend = 0.5f; fitReference(); }},
            {"1 (height)", [this] { _blend = 1.0f; fitReference(); }},
        }));

        layout();
        return true;
    }

    void update(float /*dt*/) override
    {
        layout();
    }

private:
    ElementComponent* createElement(Entity* parent, const ElementProps& props) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        ElementDesc desc{.type = props.type, .anchor = props.anchor, .pivot = props.pivot, .useInput = props.useInput};
        desc.width = props.width;
        desc.height = props.height;
        element->setup(desc);
        if (props.sprite) {
            element->setSprite(props.sprite);
            element->setSpriteFrame(props.spriteFrame);
        }
        element->setColor(props.color);
        if (props.type == ElementType::Text) {
            element->setFontResource(_font);
            element->setFontSize(props.fontSize);
            element->setText(props.text);
        }
        parent->addChild(entity);
        return element;
    }

    /// A button that shows a setting, and moves it on to its next value when pressed.
    Entity* createToggle(Entity* screen, const std::string& name, std::vector<Choice> choices)
    {
        ElementComponent* image = createElement(screen, {.sprite = _panelSprite, .color = ORANGE, .anchor = CENTRE_ANCHOR,
            .pivot = CENTRE_PIVOT, .width = 260.0f, .height = 64.0f, .useInput = true});
        auto* button = static_cast<ButtonComponent*>(image->entity()->addComponent<ButtonComponent>());
        button->setImageEntity(image->entity());
        button->setHoverTint(Color(1.0f, 0.7f, 0.45f, 1.0f));
        button->setPressedTint(Color(0.8f, 0.4f, 0.1f, 1.0f));
        ElementComponent* label = createElement(image->entity(), {.type = ElementType::Text,
            .color = Color(0.1f, 0.1f, 0.1f, 1.0f), .anchor = CENTRE_ANCHOR, .pivot = CENTRE_PIVOT, .fontSize = 24});
        auto state = std::make_shared<std::pair<std::vector<Choice>, size_t>>(std::move(choices), 0);
        const auto show = [this, label, name, state] {
            const Choice& choice = state->first[state->second];
            label->setText(name + ": " + choice.label);
            choice.apply();
            showReadout();
        };
        button->on("click", [state, show] {
            state->second = (state->second + 1) % state->first.size();
            show();
        });
        show();
        return image->entity();
    }

    void fitReference()
    {
        const Vector2 resolution = _hud->resolution();
        const Vector2 reference = _hud->referenceResolution();
        const bool wider = resolution.x / reference.x > resolution.y / reference.y;
        _hud->setScaleBlend(_blend ? *_blend : (wider ? 1.0f : 0.0f));
    }

    void showReadout()
    {
        if (!_readout) {
            return;
        }
        const auto [w, h] = engine()->graphicsDevice()->size();
        std::ostringstream text;
        text << "Canvas " << w << " × " << h << " pixels, HUD scale " << std::fixed << std::setprecision(2)
             << _hud->scale();
        _readout->setText(text.str());
    }

    // Lay the controls out along the bottom of the screen, or stacked on portrait canvases
    void layout()
    {
        const auto [w, h] = engine()->canvasSize();
        if (w == _laidOutWidth && h == _laidOutHeight) {
            return;
        }
        _laidOutWidth = w;
        _laidOutHeight = h;
        fitReference();
        const bool portrait = h > w;
        _controls->setReferenceResolution(portrait ? Vector2(540.0f, 960.0f) : Vector2(1280.0f, 720.0f));
        for (size_t i = 0; i < _toggles.size(); ++i) {
            const auto fi = static_cast<float>(i);
            _toggles[i]->setLocalPosition(portrait ? 0.0f : (fi - 0.5f) * 280.0f, portrait ? -40.0f - fi * 80.0f : -80.0f,
                                          0.0f);
        }
        _readout->entity()->setLocalPosition(0.0f, portrait ? 60.0f : 0.0f, 0.0f);
        showReadout();
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    std::shared_ptr<Sprite> _panelSprite;
    ScreenComponent* _hud = nullptr;
    ScreenComponent* _controls = nullptr;
    ElementComponent* _readout = nullptr;
    std::vector<Entity*> _toggles;
    std::optional<float> _blend;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(ScreenScalingExample)
