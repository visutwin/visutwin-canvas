// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/world-ui.
//
// A hangar: a floor, a back wall, and a front wall with an opening that a door slides up
// into, under a shadow-casting directional light, with a warm omni lamp in the room behind
// the door. Two WORLD-SPACE screens hang on the front wall: an exit sign above the door (a
// green sliced panel with "Exit") and a control panel beside it with a title, a status line
// and two tint buttons, Open and Close, which take input like the buttons of a 2D interface
// — through a ray from the camera into the screen's world corners. Only the button that can
// act is active. The camera orbits the door; drag to look around.
//
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/materials/standardMaterial.h"
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

    /// Upstream's createElement properties that this example sets.
    struct ElementProps
    {
        ElementType type = ElementType::Image;
        std::shared_ptr<Sprite> sprite;
        Color color = LIGHT;
        Vector4 anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f);
        std::optional<Vector4> margin;
        std::optional<float> width;
        std::optional<float> height;
        bool useInput = false;
        std::string text;
        int fontSize = 32;
    };
}

class WorldUiExample final: public ExampleApp
{
public:
    WorldUiExample()
        : ExampleApp({.title = "World UI", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

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

        // The hangar: a floor, and a wall with an opening that the door slides up into
        scene()->setAmbientLight(0.25f, 0.27f, 0.32f);
        createBox(Color(0.2f, 0.22f, 0.26f, 1.0f), Vector3(0.0f, -0.05f, 0.0f), Vector3(20.0f, 0.1f, 16.0f));
        createBox(Color(0.5f, 0.42f, 0.35f, 1.0f), Vector3(0.0f, 3.0f, -6.0f), Vector3(20.0f, 6.0f, 0.2f));
        createBox(Color(0.32f, 0.34f, 0.4f, 1.0f), Vector3(-5.2f, 3.0f, -0.2f), Vector3(8.0f, 6.0f, 0.4f));
        createBox(Color(0.32f, 0.34f, 0.4f, 1.0f), Vector3(5.2f, 3.0f, -0.2f), Vector3(8.0f, 6.0f, 0.4f));
        createBox(Color(0.32f, 0.34f, 0.4f, 1.0f), Vector3(0.0f, 4.5f, -0.2f), Vector3(2.4f, 3.0f, 0.4f));
        _door = createBox(Color(0.85f, 0.65f, 0.25f, 1.0f), Vector3(0.0f, 1.5f, -0.2f), Vector3(2.4f, 3.0f, 0.2f));

        auto* light = createDirectionalLight(Vector3(60.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowType(SHADOW_PCF3_32F);
            lightComp->setShadowDistance(20.0f);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
        }

        // A warm light in the room behind the door, which shows as the door opens
        auto* lamp = new Entity();
        lamp->setEngine(engine());
        if (auto* lampComp = static_cast<LightComponent*>(lamp->addComponent<LightComponent>())) {
            lampComp->setType(LightType::LIGHTTYPE_OMNI);
            lampComp->setColor(Color(1.0f, 0.75f, 0.45f, 1.0f));
            lampComp->setIntensity(2.0f);
            lampComp->setRange(8.0f);
        }
        lamp->setPosition(0.0f, 2.5f, -4.0f);
        root()->addChild(lamp);

        // A camera that orbits the door. Drag to look around
        auto* cameraEntity = createCamera(Vector3(2.6f, 2.4f, 6.8f));
        _camera = cameraEntity->findComponent<CameraComponent>();
        _camera->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));
        _camera->camera()->setFov(50.0f);
        if (auto* controls = addOrbitControls(cameraEntity, Vector3(1.0f, 2.2f, 0.0f))) {
            controls->setPitchRange(Vector2(-40.0f, 10.0f));
            controls->setZoomRange(Vector2(3.0f, 10.0f));
        }

        auto atlas = std::make_shared<TextureAtlas>();
        atlas->setTexture(atlasTexture);
        atlas->setFrame("panel", {.rect = Vector4(548.0f, 660.0f, 128.0f, 128.0f), .pivot = Vector2(0.5f, 0.5f),
                                  .border = Vector4(32.0f, 32.0f, 32.0f, 32.0f)});
        _panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        const Vector4 fill(0.0f, 0.0f, 1.0f, 1.0f);
        const Vector4 noMargin(0.0f, 0.0f, 0.0f, 0.0f);

        // The exit sign: a world-space screen 400 x 240 units, scaled to 1.6 x 0.96 meters,
        // above the door. Its elements are depth-tested against the scene
        Entity* sign = createScreen(Vector2(400.0f, 240.0f), Vector3(0.0f, 4.6f, 0.01f));
        createElement(sign, {.sprite = _panel, .color = Color(0.1f, 0.45f, 0.25f, 1.0f), .anchor = fill,
                             .margin = noMargin});
        createElement(sign, {.type = ElementType::Text, .text = "Exit", .fontSize = 96});

        // The control panel beside the door: a screen of its own, at the same scale, whose
        // buttons take input like the buttons of a 2D interface
        Entity* controls = createScreen(Vector2(300.0f, 360.0f), Vector3(2.4f, 1.6f, 0.01f));
        createElement(controls, {.sprite = _panel, .color = PANEL, .anchor = fill, .margin = noMargin});
        createElement(controls, {.type = ElementType::Text, .text = "Hangar Door", .fontSize = 36})
            ->entity()->setLocalPosition(0.0f, 130.0f, 0.0f);
        _status = createElement(controls, {.type = ElementType::Text, .color = MUTED, .fontSize = 28});
        _status->entity()->setLocalPosition(0.0f, 78.0f, 0.0f);

        _open = createButton(controls, "Open", -10.0f);
        _close = createButton(controls, "Close", -100.0f);
        _open->on("click", [this]() { setTarget(1.0f); });
        _close->on("click", [this]() { setTarget(0.0f); });
        setTarget(0.0f);
        return true;
    }

    // The door slides up into the wall, or down to close
    void update(const float dt) override
    {
        _raised = _target > _raised ? std::min(_raised + dt * 0.8f, _target) : std::max(_raised - dt * 0.8f, _target);
        _door->setPosition(0.0f, 1.5f + _raised * 2.9f, -0.2f);
        const std::string text = _raised == _target ? (_target > 0.0f ? "Open" : "Closed")
                                                    : (_target > 0.0f ? "Opening…" : "Closing…");
        if (_status->text() != text) {
            _status->setText(text);
        }

        // On portrait canvases, fit the scene to the width of the view rather than its height
        const auto [w, h] = engine()->canvasSize();
        _camera->camera()->setHorizontalFov(h > w);
    }

private:
    Entity* createBox(const Color& color, const Vector3& position, const Vector3& size)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setGloss(0.5f);
        _materials.push_back(material);
        return createPrimitive("box", material.get(), position, size);
    }

    /// A world-space screen of `resolution` units at 4 mm a unit.
    Entity* createScreen(const Vector2& resolution, const Vector3& position)
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* screen = static_cast<ScreenComponent*>(entity->addComponent<ScreenComponent>());
        screen->setScreenSpace(false);
        screen->setResolution(resolution);
        entity->setLocalScale(0.004f, 0.004f, 0.004f);
        entity->setPosition(position.getX(), position.getY(), position.getZ());
        root()->addChild(entity);
        return entity;
    }

    /// An element on `parent`, centred on it unless `props` says otherwise (upstream's
    /// createElement).
    ElementComponent* createElement(Entity* parent, const ElementProps& props) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        ElementDesc desc{.type = props.type, .anchor = props.anchor, .pivot = Vector2(0.5f, 0.5f),
                         .margin = props.margin, .useInput = props.useInput};
        desc.width = props.width;
        desc.height = props.height;
        element->setup(desc);
        if (props.sprite) {
            element->setSprite(props.sprite);
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

    ButtonComponent* createButton(Entity* controls, const std::string& text, const float y)
    {
        ElementComponent* image = createElement(controls, {.sprite = _panel, .color = ORANGE, .width = 220.0f,
                                                           .height = 72.0f, .useInput = true});
        image->entity()->setLocalPosition(0.0f, y, 0.0f);
        auto* button = static_cast<ButtonComponent*>(image->entity()->addComponent<ButtonComponent>());
        button->setImageEntity(image->entity());
        button->setHoverTint(Color(1.0f, 0.7f, 0.45f, 1.0f));
        button->setPressedTint(Color(0.8f, 0.4f, 0.1f, 1.0f));
        button->setInactiveTint(Color(0.3f, 0.32f, 0.37f, 1.0f));
        createElement(image->entity(), {.type = ElementType::Text, .color = Color(0.1f, 0.1f, 0.1f, 1.0f),
                                        .text = text, .fontSize = 32});
        return button;
    }

    /// Only the button that can act is active.
    void setTarget(const float value)
    {
        _target = value;
        _open->setActive(value == 0.0f);
        _close->setActive(value == 1.0f);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    std::shared_ptr<Sprite> _panel;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    CameraComponent* _camera = nullptr;
    Entity* _door = nullptr;
    ElementComponent* _status = nullptr;
    ButtonComponent* _open = nullptr;
    ButtonComponent* _close = nullptr;
    float _target = 0.0f;
    float _raised = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(WorldUiExample)
