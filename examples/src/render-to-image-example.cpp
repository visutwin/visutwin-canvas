// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// Port of upstream user-interface/render-to-image.
//
// A character card with a live 3D portrait. A second camera renders the character, on a layer
// of its own, into a render target, and an image element shows its texture, behind the card's
// text and buttons like any other image. Press Attack to see the portrait move.
//
// DEVIATIONS from upstream:
//  - the render texture is PIXELFORMAT_RGBA8, not SRGBA8: the engine has no sRGB pixel formats.
//    The target is written gamma-encoded and the image element decodes its texture as sRGB, so
//    the portrait comes out the same. RENDERTARGET_ORIGIN_TOP is the only origin here.
//
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiAtlas.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/animation/animationComponent.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "framework/parsers/glbContainerResource.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/sprite.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;
constexpr int LAYERID_PREVIEW = 20;

namespace
{
    const Color ORANGE(1.0f, 0.55f, 0.2f, 1.0f);
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
        bool useInput = false;
        bool mask = false;
        FontResource* font = nullptr;
        std::string text;
        std::optional<int> fontSize;
        std::optional<float> lineHeight;
        bool autoWidth = true;
        bool wrapLines = false;
        std::optional<float> verticalAlign;
    };
}

class RenderToImageExample final: public ExampleApp
{
public:
    RenderToImageExample()
        : ExampleApp({.title = "Render to Image", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
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
        _knightAsset = std::make_unique<Asset>("knight", AssetType::CONTAINER, assetPath("models/knight.glb"));
        _font = fontOf(*_fontAsset);
        FontResource* bold = fontOf(*_boldAsset);
        Texture* atlasTexture = textureOf(*_uiAtlasTexture);
        const auto knightResource = _knightAsset->resource();
        auto* knight = knightResource && std::holds_alternative<ContainerResource*>(*knightResource)
            ? dynamic_cast<GlbContainerResource*>(std::get<ContainerResource*>(*knightResource)) : nullptr;
        if (!_font || !bold || !atlasTexture || !knight) {
            spdlog::error("Failed to load the Roboto fonts, ui/ui-atlas.png or models/knight.glb");
            return false;
        }

        // The main camera draws the interface
        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.11f, 0.13f, 1.0f));

        // The texture the portrait is rendered into, and a layer for what the preview camera sees,
        // which the main camera does not render
        TextureOptions textureOptions;
        textureOptions.name = "preview";
        textureOptions.width = 512;
        textureOptions.height = 512;
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = false;
        _previewTexture = std::make_shared<Texture>(device().get(), textureOptions);
        _previewTexture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        _previewTexture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        RenderTargetOptions rtOptions;
        rtOptions.graphicsDevice = device().get();
        rtOptions.name = "preview";
        rtOptions.colorBuffer = _previewTexture.get();
        rtOptions.depth = true;
        const auto renderTarget = device()->createRenderTarget(rtOptions);
        const auto previewLayer = std::make_shared<Layer>("Preview", LAYERID_PREVIEW);
        scene()->layers()->pushOpaque(previewLayer);
        scene()->layers()->pushTransparent(previewLayer);

        // The preview camera renders before the main camera, whose priority is 0, so the texture is
        // ready when the interface is drawn. Its transparent clear color keeps the portrait's
        // background clear
        auto* previewCamera = createCamera(Vector3(0.0f, 1.1f, 5.2f));
        previewCamera->setName("preview camera");
        auto* previewCameraComp = previewCamera->findComponent<CameraComponent>();
        previewCameraComp->setLayers({LAYERID_PREVIEW});
        previewCameraComp->setPriority(-1);
        previewCameraComp->camera()->setRenderTarget(renderTarget);
        previewCameraComp->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 0.0f));
        previewCameraComp->camera()->setFov(30.0f);
        previewCamera->lookAt(Vector3(0.0f, 1.0f, 0.0f));

        // The character, and a light, on the preview layer only. The model holds its animations
        _character = knight->instantiateRenderEntity();
        _character->setEngine(engine());
        root()->addChild(_character);
        // The container plays its first clip on a legacy animation component when it has clips;
        // the anim component below drives the character instead
        if (auto* legacy = _character->findComponent<AnimationComponent>()) {
            legacy->setPlaying(false);
            legacy->setEnabled(false);
        }
        for (auto* render : _character->findComponents<RenderComponent>()) {
            render->setLayers({LAYERID_PREVIEW});
        }
        const auto& tracks = knight->animTracks();
        const auto idle = tracks.find("Idle_FightingStance");
        const auto attack = tracks.find("Attack_SwordThrust");
        if (idle == tracks.end() || attack == tracks.end()) {
            spdlog::error("models/knight.glb has no Idle_FightingStance or Attack_SwordThrust animation");
            return false;
        }
        _attackDuration = attack->second->duration();
        _anim = static_cast<AnimComponent*>(_character->addComponent<AnimComponent>());
        _anim->setActivate(true);
        _anim->assignAnimation("Idle", idle->second);
        _anim->assignAnimation("Attack", attack->second, {}, 1.0f, false);

        auto* light = new Entity();
        light->setEngine(engine());
        light->setName("light");
        auto* lightComp = static_cast<LightComponent*>(light->addComponent<LightComponent>());
        lightComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        lightComp->setLayers({LAYERID_PREVIEW});
        lightComp->setIntensity(1.2f);
        light->setLocalEulerAngles(40.0f, 30.0f, 0.0f);
        root()->addChild(light);
        scene()->setAmbientLight(0.45f, 0.47f, 0.55f);

        // The interface
        auto* screenEntity = new Entity();
        screenEntity->setEngine(engine());
        _screen = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
        _screen->setScreenSpace(true);
        _screen->setReferenceResolution(Vector2(1280.0f, 720.0f));
        _screen->setScaleMode(ScreenScaleMode::Blend);
        _screen->setScaleBlend(0.5f);
        root()->addChild(screenEntity);

        auto atlas = createUiAtlas(atlasTexture);
        auto panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        const Vector4 topLeft(0.0f, 1.0f, 0.0f, 1.0f);
        const Vector2 topLeftPivot(0.0f, 1.0f);

        // The card: a frame whose mask crops the portrait, an image of the rendered texture, and the
        // character's details
        _card = createElement(screenEntity, {.sprite = panel, .color = PANEL, .width = 720.0f, .height = 440.0f});
        _frame = createElement(_card->entity(), {.sprite = panel, .color = Color(0.3f, 0.35f, 0.5f, 1.0f),
                                                 .width = 360.0f, .height = 400.0f});
        ElementComponent* mask = createElement(_frame->entity(), {.sprite = panel,
            .anchor = Vector4(0.0f, 0.0f, 1.0f, 1.0f), .margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f), .mask = true});
        createElement(mask->entity(), {.texture = _previewTexture.get(), .width = 400.0f, .height = 400.0f});

        _details = createElement(_card->entity(), {.type = ElementType::Group, .pivot = topLeftPivot});
        createElement(_details->entity(), {.type = ElementType::Text, .anchor = topLeft, .pivot = topLeftPivot,
                                           .font = bold, .text = "Rowan", .fontSize = 48});
        createElement(_details->entity(), {.type = ElementType::Text, .color = ORANGE, .anchor = topLeft,
                                           .pivot = topLeftPivot, .text = "Level 12 · Knight", .fontSize = 26})
            ->entity()->setLocalPosition(0.0f, -64.0f, 0.0f);
        createElement(_details->entity(), {.type = ElementType::Text, .color = MUTED, .anchor = topLeft,
            .pivot = topLeftPivot, .width = 280.0f,
            .text = "First through every crypt door, and last out of every fight. Never lowers his guard.",
            .fontSize = 22, .lineHeight = 30.0f, .autoWidth = false, .wrapLines = true, .verticalAlign = 1.0f})
            ->entity()->setLocalPosition(0.0f, -110.0f, 0.0f);

        // Attack plays the thrust once, and the character goes back to idling when it ends
        _attack = createElement(_card->entity(), {.sprite = panel, .color = ORANGE, .width = 220.0f, .height = 64.0f,
                                                  .useInput = true});
        auto* button = static_cast<ButtonComponent*>(_attack->entity()->addComponent<ButtonComponent>());
        button->setImageEntity(_attack->entity());
        button->setHoverTint(Color(1.0f, 0.7f, 0.45f, 1.0f));
        button->setPressedTint(Color(0.8f, 0.4f, 0.1f, 1.0f));
        createElement(_attack->entity(), {.type = ElementType::Text, .color = Color(0.1f, 0.1f, 0.1f, 1.0f),
                                          .font = bold, .text = "Attack", .fontSize = 28});
        button->on("click", [this]() {
            _anim->baseLayer()->transition("Attack", 0.2f);
            _idleIn = _attackDuration;
        });

        layout();
        return true;
    }

    // Turn the character slowly, and go back to idling after the attack
    void update(const float dt) override
    {
        layout();
        _character->rotateLocal(0.0f, dt * 20.0f, 0.0f);
        if (_idleIn > 0.0f) {
            _idleIn -= dt;
            if (_idleIn <= 0.0f) {
                _anim->baseLayer()->transition("Idle", 0.3f);
            }
        }
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
        desc.useInput = props.useInput;
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
            if (props.fontSize) {
                element->setFontSize(*props.fontSize);
            }
            if (props.lineHeight) {
                element->setLineHeight(*props.lineHeight);
            }
            if (props.verticalAlign) {
                element->setHorizontalAlign(ElementHorizontalAlign::Left);
                element->setVerticalAlign(*props.verticalAlign);
            }
            // A text that wraps turns autoWidth off before its text is set, and takes its width
            // after: with autoWidth on, the empty text had sized it to nothing
            element->setAutoWidth(props.autoWidth);
            if (!props.autoWidth && props.width) {
                element->setWidth(*props.width);
            }
            element->setWrapLines(props.wrapLines);
            element->setText(props.text);
        }
        parent->addChild(entity);
        return element;
    }

    // The portrait beside the details on landscape canvases, and above them on portrait ones
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
        _card->setWidth(portrait ? 480.0f : 720.0f);
        _card->setHeight(portrait ? 820.0f : 440.0f);
        _frame->entity()->setLocalPosition(portrait ? 0.0f : -160.0f, portrait ? 190.0f : 0.0f, 0.0f);
        _details->entity()->setLocalPosition(portrait ? -200.0f : 40.0f, portrait ? -40.0f : 180.0f, 0.0f);
        _attack->entity()->setLocalPosition(portrait ? 0.0f : 150.0f, portrait ? -330.0f : -160.0f, 0.0f);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    std::unique_ptr<Asset> _knightAsset;
    std::shared_ptr<Texture> _previewTexture;
    FontResource* _font = nullptr;
    ScreenComponent* _screen = nullptr;
    Entity* _character = nullptr;
    AnimComponent* _anim = nullptr;
    float _attackDuration = 0.0f;
    float _idleIn = 0.0f;
    ElementComponent* _card = nullptr;
    ElementComponent* _frame = nullptr;
    ElementComponent* _details = nullptr;
    ElementComponent* _attack = nullptr;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(RenderToImageExample)
