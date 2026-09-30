// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/particle-system.
//
// A daily reward card with a sparkling chest. Its particle systems are in screen space and on
// the UI layer, so they are drawn in the hierarchy's order with the elements: over the card and
// the chest, and under the text and button that come after them. Claim the reward for a burst.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiAtlas.h"
#include "core/math/curve.h"
#include "core/math/curveSet.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/constants.h"
#include "scene/sprite.h"

using namespace visutwin::canvas;

namespace
{
    const Color ORANGE(1.0f, 0.55f, 0.2f, 1.0f);
    const Color GOLD(1.0f, 0.8f, 0.3f, 1.0f);
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

    // Number.toLocaleString('en-US') for a whole number: thousands separated by commas.
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
        Vector4 anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f);
        Vector2 pivot = Vector2(0.5f, 0.5f);
        std::shared_ptr<Sprite> sprite;
        int spriteFrame = 0;
        Color color = LIGHT;
        std::optional<float> width;
        std::optional<float> height;
        bool useInput = false;
        FontResource* font = nullptr;
        std::string text;
        std::optional<int> fontSize;
    };
}

class UiParticleSystemExample final: public ExampleApp
{
public:
    UiParticleSystemExample() : ExampleApp({.title = "UI Particle System"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ScreenComponentSystem>();
        options.registerComponentSystem<ElementComponentSystem>();
        options.registerComponentSystem<ButtonComponentSystem>();
        options.registerComponentSystem<ParticleSystemComponentSystem>();
        _elementInput = std::make_shared<ElementInput>();
        options.elementInput = _elementInput;
    }

    bool create() override
    {
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _boldAsset = std::make_unique<Asset>("bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        _sparkAsset = std::make_unique<Asset>("spark", AssetType::TEXTURE, assetPath("textures/spark.png"));
        _font = fontOf(*_fontAsset);
        _bold = fontOf(*_boldAsset);
        Texture* atlasTexture = textureOf(*_uiAtlasTexture);
        Texture* spark = textureOf(*_sparkAsset);
        if (!_font || !_bold || !atlasTexture || !spark) {
            spdlog::error("Failed to load the Roboto fonts, ui/ui-atlas.png or textures/spark.png");
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
        auto panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        auto icons = std::make_shared<Sprite>(atlas, std::vector<std::string>{"icon-chest", "icon-coin"});

        // The player's coins, in the top-right corner
        ElementComponent* purse = createElement(_screenEntity, {.type = ElementType::Group,
            .anchor = Vector4(1.0f, 1.0f, 1.0f, 1.0f), .pivot = Vector2(1.0f, 1.0f)});
        purse->entity()->setLocalPosition(-40.0f, -40.0f, 0.0f);
        createElement(purse->entity(), {.sprite = icons, .spriteFrame = 1, .color = GOLD, .width = 48.0f,
                                        .height = 48.0f});
        _coins = createElement(purse->entity(), {.type = ElementType::Text, .anchor = Vector4(1.0f, 0.5f, 1.0f, 0.5f),
            .pivot = Vector2(1.0f, 0.5f), .font = _bold, .fontSize = 36});
        _coins->entity()->setLocalPosition(-40.0f, 0.0f, 0.0f);

        // The card, and the chest in the middle of it
        ElementComponent* card = createElement(_screenEntity, {.sprite = panel, .color = PANEL, .width = 420.0f,
                                                               .height = 500.0f});
        createElement(card->entity(), {.type = ElementType::Text, .font = _bold, .text = "Daily Reward",
                                       .fontSize = 36})->entity()->setLocalPosition(0.0f, 200.0f, 0.0f);
        _day = createElement(card->entity(), {.type = ElementType::Text, .color = MUTED, .font = _font,
                                              .fontSize = 24});
        _day->entity()->setLocalPosition(0.0f, 158.0f, 0.0f);
        createElement(card->entity(), {.sprite = icons, .spriteFrame = 0, .color = GOLD, .width = 160.0f,
                                       .height = 160.0f})->entity()->setLocalPosition(0.0f, 30.0f, 0.0f);

        // The particles are drawn with the elements, in the order of the hierarchy, when they are in
        // screen space and on the UI layer. Sparkles twinkle around the chest while the reward waits
        const CurveSet gold({{0.0f, 1.0f}, {0.0f, 0.8f}, {0.0f, 0.4f}});
        _sparkles = createParticles(card->entity(), [&](ParticleEmitterOptions& o) {
            o.numParticles = 20;
            o.lifetime = 1.2f;
            o.lifetime2 = 1.2f;
            o.rate = 0.06f;
            o.preWarm = true;
            o.emitterRadius = 220.0f;
            o.colorMap = spark;
            o.scaleGraph = Curve(std::vector<float>{0.0f, 0.0f, 0.4f, 0.04f, 1.0f, 0.0f});
            o.rotationSpeedGraph = Curve(std::vector<float>{0.0f, 90.0f});
            o.colorGraph = gold;
        });

        // Claiming bursts 50 sparks out of the chest, once
        _burst = createParticles(card->entity(), [&](ParticleEmitterOptions& o) {
            o.numParticles = 50;
            o.lifetime = 1.0f;
            o.lifetime2 = 1.0f;
            o.rate = 0.0f;
            o.loop = false;
            o.autoPlay = false;
            o.emitterRadius = 30.0f;
            o.colorMap = spark;
            o.alignToMotion = true;
            o.stretch = 0.3f;
            o.localVelocityGraph = CurveSet({{0.0f, -500.0f}, {0.0f, 100.0f, 1.0f, -600.0f}, {0.0f, 0.0f}});
            o.localVelocityGraph2 = CurveSet({{0.0f, 500.0f}, {0.0f, 700.0f, 1.0f, 0.0f}, {0.0f, 0.0f}});
            o.scaleGraph = Curve(std::vector<float>{0.0f, 0.03f, 1.0f, 0.0f});
            o.colorGraph = gold;
        });

        // The text and the button come after the particles in the hierarchy, so they are drawn over
        // them. Sync the screen's draw order after adding particles to it
        _reward = createElement(card->entity(), {.type = ElementType::Text, .color = ORANGE, .font = _bold,
                                                 .fontSize = 36});
        _reward->entity()->setLocalPosition(0.0f, -100.0f, 0.0f);
        ElementComponent* claim = createElement(card->entity(), {.sprite = panel, .color = ORANGE, .width = 240.0f,
                                                                 .height = 64.0f, .useInput = true});
        claim->entity()->setLocalPosition(0.0f, -180.0f, 0.0f);
        _claim = static_cast<ButtonComponent*>(claim->entity()->addComponent<ButtonComponent>());
        _claim->setImageEntity(claim->entity());
        _claim->setHoverTint(Color(1.0f, 0.7f, 0.45f, 1.0f));
        _claim->setPressedTint(Color(0.8f, 0.4f, 0.1f, 1.0f));
        _claim->setInactiveTint(Color(0.3f, 0.32f, 0.37f, 1.0f));
        _label = createElement(claim->entity(), {.type = ElementType::Text, .color = Color(0.1f, 0.1f, 0.1f, 1.0f),
                                                 .font = _bold, .fontSize = 28});
        _screen->syncDrawOrder();

        _coins->setText(withCommas(_total));
        offer();

        // Claiming bursts the sparks, stops the twinkling and adds the coins. The next reward is
        // offered three seconds later
        _claim->on("click", [this]() {
            _burst->reset();
            _burst->play();
            _sparkles->stop();
            _total += 200 + _today * 10;
            _today++;
            _label->setText("Claimed");
            _claim->setActive(false);
            _next = 3.0f;
        });

        layout();
        return true;
    }

    // Count the coins up to the new total, and offer the next reward when it is due
    void update(const float dt) override
    {
        if (_shown < static_cast<float>(_total)) {
            _shown = std::min(_shown + dt * 250.0f, static_cast<float>(_total));
            _coins->setText(withCommas(static_cast<int>(std::floor(_shown))));
        }
        if (_next > 0.0f) {
            _next -= dt;
            if (_next <= 0.0f) {
                offer();
            }
        }
        layout();
    }

private:
    ElementComponent* createElement(Entity* parent, const ElementProps& props) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        ElementDesc desc{.type = props.type, .anchor = props.anchor, .pivot = props.pivot};
        desc.width = props.width;
        desc.height = props.height;
        desc.useInput = props.useInput;
        element->setup(desc);
        if (props.sprite) {
            element->setSprite(props.sprite);
            element->setSpriteFrame(props.spriteFrame);
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

    // A screen-space, local-space particle system on the UI layer, 30 units above the card's
    // centre where the chest is, emitting additively from a sphere
    template <typename Configure>
    ParticleSystemComponent* createParticles(Entity* card, Configure configure) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        card->addChild(entity);
        entity->setLocalPosition(0.0f, 30.0f, 0.0f);
        auto* particles = static_cast<ParticleSystemComponent*>(entity->addComponent<ParticleSystemComponent>());
        auto& o = particles->options();
        o.screenSpace = true;
        o.localSpace = true;
        o.layers = {LAYERID_UI};
        o.blendType = ParticleBlendType::BLEND_ADDITIVE;
        o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
        configure(o);
        particles->apply();
        return particles;
    }

    // Offer the reward for a day. Each day's reward is 50 coins more than the last
    void offer()
    {
        _day->setText("Day " + std::to_string(_today));
        _reward->setText("+" + std::to_string(200 + _today * 10) + " coins");
        _label->setText("Claim");
        _claim->setActive(true);
        _sparkles->play();
    }

    // Use a portrait reference resolution on portrait canvases, and scale to whichever axis has
    // less room, so the card stays on screen
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
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    std::unique_ptr<Asset> _sparkAsset;
    FontResource* _font = nullptr;
    FontResource* _bold = nullptr;
    Entity* _screenEntity = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _coins = nullptr;
    ElementComponent* _day = nullptr;
    ElementComponent* _reward = nullptr;
    ElementComponent* _label = nullptr;
    ButtonComponent* _claim = nullptr;
    ParticleSystemComponent* _sparkles = nullptr;
    ParticleSystemComponent* _burst = nullptr;
    int _total = 1250;
    float _shown = 1250.0f;
    int _today = 5;
    float _next = 0.0f;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(UiParticleSystemExample)
