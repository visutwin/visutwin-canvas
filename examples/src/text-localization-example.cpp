// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/text-localization.
//
// A shop card in four languages. Text elements with a localization key show the message of the
// current locale. A message with a number picks its plural form with getPluralText, the price is
// formatted for the locale, and Français asks for fr-CA, which falls back to fr-FR.
//
// DEVIATION: upstream formats the price with the browser's Intl.NumberFormat, which C++ does not
// have. formatPrice below writes exactly what Intl writes for the four locales and currencies the
// buttons can reach (checked against a browser): "$0.99", and "0,99 €" / "0,99 zł" with a
// no-break space (U+00A0) before the symbol.
//
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../exampleApp.h"
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
#include "framework/i18n/i18n.h"
#include "framework/input/elementInput.h"
#include "scene/sprite.h"

using namespace visutwin::canvas;

namespace
{
    const Color ORANGE(1.0f, 0.55f, 0.2f, 1.0f);
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

    // Intl.NumberFormat(locale, {style: 'currency', currency}).format(value) for the locales and
    // currencies this example reaches; see the header.
    std::string formatPrice(const std::string& locale, const std::string& currency, const double value)
    {
        const long cents = std::lround(value * 100.0);
        const std::string whole = std::to_string(cents / 100);
        const std::string fraction = (cents % 100 < 10 ? "0" : "") + std::to_string(cents % 100);
        if (I18n::getLang(locale) == "en") {
            return (currency == "USD" ? "$" : currency + " ") + whole + "." + fraction;
        }
        const std::string symbol = currency == "EUR" ? "€" : currency == "PLN" ? "zł" : currency;
        return whole + "," + fraction + " " + symbol;
    }

    std::string replaceNumber(std::string text, const int number)
    {
        const std::string token = "{number}";
        if (const size_t at = text.find(token); at != std::string::npos) {
            text.replace(at, token.size(), std::to_string(number));
        }
        return text;
    }

    struct ElementProps
    {
        ElementType type = ElementType::Image;
        Vector4 anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f);
        Vector2 pivot = Vector2(0.5f, 0.5f);
        std::optional<Vector4> margin;
        std::shared_ptr<Sprite> sprite;
        Color color = LIGHT;
        std::optional<float> width;
        std::optional<float> height;
        bool useInput = false;
        FontResource* font = nullptr;
        std::string text;
        std::string key;
        std::optional<int> fontSize;
    };
}

class TextLocalizationExample final: public ExampleApp
{
public:
    TextLocalizationExample() : ExampleApp({.title = "Text Localization", .width = 1280, .height = 720}) {}

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

        // The localization data is already here, so it is added directly rather than loaded
        _i18n = engine()->i18n();
        if (!_i18n->addDataFromFile(assetPath("localization/text-localization.json"))) {
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
        const auto sprite = [&](const std::string& frame) {
            return std::make_shared<Sprite>(atlas, std::vector<std::string>{frame}, 2.0f, SpriteRenderMode::Sliced);
        };
        _panel = sprite("panel");
        _outline = sprite("panel-outline");
        auto coin = std::make_shared<Sprite>(atlas, std::vector<std::string>{"icon-coin"});

        ElementComponent* card = createElement(_screenEntity, {.sprite = _panel, .color = Color(0.16f, 0.18f, 0.23f, 1.0f),
                                                               .width = 500.0f, .height = 400.0f});
        card->entity()->setLocalPosition(0.0f, 70.0f, 0.0f);
        createElement(card->entity(), {.sprite = coin, .color = Color(1.0f, 0.8f, 0.3f, 1.0f), .width = 64.0f,
                                       .height = 64.0f})->entity()->setLocalPosition(-190.0f, 50.0f, 0.0f);

        // Localized text: each element shows the message of its key, in the current locale
        const ElementProps text{.type = ElementType::Text, .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f),
                                .pivot = Vector2(0.0f, 1.0f)};
        auto props = [&](ElementProps p) { return p; };
        ElementProps titleProps = props(text);
        titleProps.font = _bold;
        titleProps.key = "title";
        titleProps.fontSize = 36;
        createElement(card->entity(), titleProps)->entity()->setLocalPosition(32.0f, -28.0f, 0.0f);
        ElementProps itemProps = props(text);
        itemProps.font = _bold;
        itemProps.key = "item";
        itemProps.fontSize = 28;
        createElement(card->entity(), itemProps)->entity()->setLocalPosition(112.0f, -110.0f, 0.0f);
        ElementProps descriptionProps = props(text);
        descriptionProps.key = "description";
        descriptionProps.fontSize = 22;
        descriptionProps.color = MUTED;
        createElement(card->entity(), descriptionProps)->entity()->setLocalPosition(112.0f, -150.0f, 0.0f);

        // Text with a number, and the number itself, are formatted in a script
        ElementProps priceProps = props(text);
        priceProps.font = _bold;
        priceProps.fontSize = 26;
        priceProps.color = ORANGE;
        _price = createElement(card->entity(), priceProps);
        _price->entity()->setLocalPosition(112.0f, -190.0f, 0.0f);
        _purse = createElement(card->entity(), {.type = ElementType::Text, .color = MUTED, .fontSize = 24});
        _purse->entity()->setLocalPosition(0.0f, -150.0f, 0.0f);

        ElementComponent* buy = createButton(card->entity(), {.color = Color(0.1f, 0.1f, 0.1f, 1.0f), .key = "buy"},
                                             200.0f, ORANGE);
        buy->entity()->setLocalPosition(0.0f, -90.0f, 0.0f);
        chosen(buy)->entity()->setEnabled(false);

        // The locales of the language buttons. There is no data for fr-CA, so it uses fr-FR
        const std::array<std::pair<const char*, const char*>, 4> languages{
            {{"English", "en-US"}, {"Français", "fr-CA"}, {"Español", "es-ES"}, {"Polski", "pl-PL"}}};
        for (const auto& [name, locale] : languages) {
            ElementComponent* button = createButton(_screenEntity, {.text = name}, 150.0f,
                                                    Color(0.2f, 0.23f, 0.29f, 1.0f));
            const std::string target = locale;
            buttonOf(button)->on("click", [this, target]() { _i18n->setLocale(target); });
            _languages.push_back({button, locale});
        }
        _caption = createElement(_screenEntity, {.type = ElementType::Text, .color = MUTED, .fontSize = 22});

        // Everything that isn't a localized text element is updated from the locale's change event
        _i18n->on("change", [this](const std::string& /*locale*/, const std::string& /*old*/) { refresh(); });
        refresh();

        buttonOf(buy)->on("click", [this]() {
            ++_coins;
            refresh();
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
        }
        element->setColor(props.color);
        if (props.type == ElementType::Text) {
            element->setFontResource(props.font ? props.font : _font);
            if (props.fontSize) {
                element->setFontSize(*props.fontSize);
            }
            if (!props.key.empty()) {
                element->setKey(props.key);
            } else {
                element->setText(props.text);
            }
        }
        parent->addChild(entity);
        return element;
    }

    // A button with a text element. The label is either a localization key or a plain text.
    ElementComponent* createButton(Entity* parent, ElementProps label, const float width, const Color& color)
    {
        ElementComponent* button = createElement(parent, {.sprite = _panel, .color = color, .width = width,
                                                          .height = 60.0f, .useInput = true});
        auto* component = static_cast<ButtonComponent*>(button->entity()->addComponent<ButtonComponent>());
        component->setImageEntity(button->entity());
        component->setHoverTint(Color(color.r * 0.8f + 0.2f, color.g * 0.8f + 0.2f, color.b * 0.8f + 0.2f, 1.0f));
        component->setPressedTint(Color(color.r * 0.7f, color.g * 0.7f, color.b * 0.7f, 1.0f));
        _chosen[button] = createElement(button->entity(), {.anchor = Vector4(0.0f, 0.0f, 1.0f, 1.0f),
            .margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f), .sprite = _outline, .color = ORANGE});
        label.type = ElementType::Text;
        label.font = _bold;
        label.fontSize = 24;
        createElement(button->entity(), label);
        return button;
    }

    static ButtonComponent* buttonOf(const ElementComponent* element)
    {
        return element->entity()->findComponent<ButtonComponent>();
    }

    ElementComponent* chosen(ElementComponent* button) { return _chosen[button]; }

    void refresh()
    {
        const std::string& locale = _i18n->locale();
        const std::string available = _i18n->findAvailableLocale(locale);
        static const std::map<std::string, std::string> currencies{
            {"en-US", "USD"}, {"fr-FR", "EUR"}, {"es-ES", "EUR"}, {"pl-PL", "PLN"}};
        const auto currency = currencies.find(available);
        _purse->setText(replaceNumber(_i18n->getPluralText("coins", _coins), _coins));
        _price->setText(formatPrice(locale, currency != currencies.end() ? currency->second : "USD", 0.99));
        _caption->setText(available == locale ? "Locale " + locale : "Locale " + locale + ", using " + available);
        for (const auto& [button, buttonLocale] : _languages) {
            chosen(button)->entity()->setEnabled(buttonLocale == locale);
        }
    }

    // The language buttons in a row, on landscape canvases, and two rows on portrait ones
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
        for (size_t i = 0; i < _languages.size(); ++i) {
            const float fi = static_cast<float>(i);
            const float x = portrait ? (static_cast<float>(i % 2) - 0.5f) * 170.0f : (fi - 1.5f) * 170.0f;
            _languages[i].first->entity()->setLocalPosition(x, portrait ? -180.0f - std::floor(fi / 2.0f) * 76.0f
                                                                        : -190.0f, 0.0f);
        }
        _caption->entity()->setLocalPosition(0.0f, portrait ? -350.0f : -260.0f, 0.0f);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    FontResource* _font = nullptr;
    FontResource* _bold = nullptr;
    I18n* _i18n = nullptr;
    std::shared_ptr<Sprite> _panel;
    std::shared_ptr<Sprite> _outline;
    Entity* _screenEntity = nullptr;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _price = nullptr;
    ElementComponent* _purse = nullptr;
    ElementComponent* _caption = nullptr;
    std::map<ElementComponent*, ElementComponent*> _chosen;
    std::vector<std::pair<ElementComponent*, std::string>> _languages;
    int _coins = 1;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(TextLocalizationExample)
