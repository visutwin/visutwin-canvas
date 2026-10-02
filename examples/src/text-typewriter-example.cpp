// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 29.09.2026
//
// Port of upstream user-interface/text-typewriter.
//
// An NPC's dialog box that reveals each line letter by letter, over a picture that covers the
// screen whatever its shape. Changing a text element's rangeEnd draws part of its text without
// laying it out again. Click the box to finish a line, and again to read the next one; an
// arrow bobs once a line has been read to the end.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
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

    const std::array<const char*, 3> LINES{{
        "It is dangerous to go alone.",
        "Take this sword. It served me well for many years, and I have no more need of it.",
        "The cave to the north is full of bats. Keep your torch lit, and do not run.",
    }};
}

class TextTypewriterExample final: public ExampleApp
{
public:
    TextTypewriterExample()
        : ExampleApp({.title = "Text Typewriter", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ScreenComponentSystem>();
        options.registerComponentSystem<ElementComponentSystem>();
        _elementInput = std::make_shared<ElementInput>();
        options.elementInput = _elementInput;
    }

    bool create() override
    {
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        _boldAsset = std::make_unique<Asset>("bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _uiAtlasTexture = std::make_unique<Asset>("ui", AssetType::TEXTURE, assetPath("ui/ui-atlas.png"),
            AssetData{.mipmaps = true});
        _landscapeTexture = std::make_unique<Asset>("landscape", AssetType::TEXTURE, assetPath("ui/landscape.png"),
            AssetData{.mipmaps = true});
        FontResource* font = fontOf(*_fontAsset);
        FontResource* bold = fontOf(*_boldAsset);
        Texture* atlasTexture = textureOf(*_uiAtlasTexture);
        Texture* landscape = textureOf(*_landscapeTexture);
        if (!font || !bold || !atlasTexture || !landscape) {
            spdlog::error("Failed to load the Roboto fonts, ui/ui-atlas.png or ui/landscape.png");
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

        // Frames of the UI kit atlas
        auto atlas = std::make_shared<TextureAtlas>();
        atlas->setTexture(atlasTexture);
        const Vector2 centre(0.5f, 0.5f);
        const Vector4 none(0.0f, 0.0f, 0.0f, 0.0f);
        atlas->setFrame("panel", {.rect = Vector4(548.0f, 660.0f, 128.0f, 128.0f), .pivot = centre,
                                  .border = Vector4(32.0f, 32.0f, 32.0f, 32.0f)});
        atlas->setFrame("avatar-3", {.rect = Vector4(508.0f, 892.0f, 128.0f, 128.0f), .pivot = centre, .border = none});
        atlas->setFrame("icon-down", {.rect = Vector4(524.0f, 556.0f, 96.0f, 96.0f), .pivot = centre, .border = none});
        auto panel = std::make_shared<Sprite>(atlas, std::vector<std::string>{"panel"}, 2.0f, SpriteRenderMode::Sliced);
        auto portrait = std::make_shared<Sprite>(atlas, std::vector<std::string>{"avatar-3"});
        auto arrow = std::make_shared<Sprite>(atlas, std::vector<std::string>{"icon-down"});

        // The scene behind the dialog: a picture that covers the screen, whatever its shape
        ElementComponent* backdrop = createElement(screenEntity, {.type = ElementType::Image,
            .anchor = Vector4(0.0f, 0.0f, 1.0f, 1.0f), .margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f)});
        backdrop->setTexture(landscape);
        backdrop->setFitMode(ElementFitMode::Cover);
        backdrop->setColor(Color(0.7f, 0.7f, 0.7f, 1.0f));

        // The dialog box, at the bottom of the screen. It receives input, so it can be clicked
        _box = createElement(screenEntity, {.type = ElementType::Image, .anchor = Vector4(0.5f, 0.0f, 0.5f, 0.0f),
                                            .pivot = Vector2(0.5f, 0.0f), .useInput = true});
        _box->setSprite(panel);
        _box->setColor(Color(0.16f, 0.18f, 0.23f, 1.0f));

        _face = createElement(_box->entity(), {.type = ElementType::Image, .anchor = Vector4(0.0f, 0.5f, 0.0f, 0.5f),
                                               .pivot = Vector2(0.0f, 0.5f), .width = 130.0f, .height = 130.0f});
        _face->setSprite(portrait);
        _face->entity()->setLocalPosition(32.0f, 0.0f, 0.0f);
        _speaker = createElement(_box->entity(), {.type = ElementType::Text, .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f),
                                                  .pivot = Vector2(0.0f, 1.0f)});
        _speaker->setFontResource(bold);
        _speaker->setFontSize(28);
        _speaker->setColor(Color(1.0f, 0.55f, 0.2f, 1.0f));
        _speaker->setText("Old Man");

        // The line being read: it wraps within its width, and grows downwards from its top edge
        _dialog = createElement(_box->entity(), {.type = ElementType::Text, .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f),
                                                 .pivot = Vector2(0.0f, 1.0f)});
        _dialog->setAutoWidth(false);
        _dialog->setWrapLines(true);
        _dialog->setHorizontalAlign(ElementHorizontalAlign::Left);
        _dialog->setVerticalAlign(1.0f);
        _dialog->setLineHeight(40.0f);
        _dialog->setFontResource(font);
        _dialog->setFontSize(30);
        _dialog->setColor(Color(0.95f, 0.96f, 0.98f, 1.0f));

        // Shown when a line has been read to the end
        _more = createElement(_box->entity(), {.type = ElementType::Image, .anchor = Vector4(1.0f, 0.0f, 1.0f, 0.0f),
                                               .pivot = Vector2(1.0f, 0.0f), .width = 32.0f, .height = 32.0f});
        _more->setSprite(arrow);
        _more->setColor(Color(1.0f, 0.55f, 0.2f, 1.0f));

        // A click finishes the line, or moves on to the next one once it is finished
        _box->on("click", [this] {
            if (_shown < static_cast<float>(_length)) {
                _shown = static_cast<float>(_length);
            } else {
                nextLine();
            }
        });

        layout();
        nextLine();
        return true;
    }

    // Reveal 20 characters a second, and bob the arrow once the line is finished
    void update(const float dt) override
    {
        layout();
        _time += dt;
        _shown = std::min(_shown + dt * 20.0f, static_cast<float>(_length));
        _dialog->setRangeEnd(static_cast<int>(std::floor(_shown)));
        const bool finished = _shown >= static_cast<float>(_length);
        _more->entity()->setEnabled(finished);
        _more->entity()->setLocalPosition(-28.0f, 22.0f + std::abs(std::sin(_time * 5.0f)) * 8.0f, 0.0f);
    }

private:
    ElementComponent* createElement(Entity* parent, const ElementDesc& desc) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        element->setup(desc);
        parent->addChild(entity);
        return element;
    }

    // Show the next line. Setting the text resets the range to the whole of the new text,
    // which gives its length, and the reveal starts again from nothing
    void nextLine()
    {
        _line = (_line + 1) % LINES.size();
        _dialog->setText(LINES[_line]);
        _length = _dialog->rangeEnd();
        _shown = 0.0f;
        _dialog->setRangeEnd(0);
        _more->entity()->setEnabled(false);
    }

    // The box spans most of a landscape canvas. Portrait canvases get a compact box without
    // the portrait, which leaves room for the text
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
        _box->entity()->setLocalPosition(0.0f, 40.0f, 0.0f);
        _box->setWidth(portrait ? 500.0f : 900.0f);
        _box->setHeight(portrait ? 250.0f : 210.0f);
        _face->entity()->setEnabled(!portrait);
        const float left = portrait ? 28.0f : 190.0f;
        _speaker->entity()->setLocalPosition(left, -26.0f, 0.0f);
        _dialog->entity()->setLocalPosition(left, -66.0f, 0.0f);
        // A new width lays the text out again, which resets the range; update() sets it back
        _dialog->setWidth(portrait ? 444.0f : 650.0f);
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _fontAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _uiAtlasTexture;
    std::unique_ptr<Asset> _landscapeTexture;
    ScreenComponent* _screen = nullptr;
    ElementComponent* _box = nullptr;
    ElementComponent* _face = nullptr;
    ElementComponent* _speaker = nullptr;
    ElementComponent* _dialog = nullptr;
    ElementComponent* _more = nullptr;
    size_t _line = LINES.size() - 1;   // nextLine() moves on to the first
    int _length = 0;
    float _shown = 0.0f;
    float _time = 0.0f;
    int _laidOutWidth = -1;
    int _laidOutHeight = -1;
};

VISUTWIN_EXAMPLE_MAIN(TextTypewriterExample)
