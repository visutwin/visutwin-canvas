// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/text.
//
// A screen-space Screen (reference resolution 1280x720) over a dark grey clear
// holds four Text elements in the courier MSDF font, centred horizontally
// and placed relative to the screen centre: "Basic Text" (42 px, 200 up), a
// wrapped rainbow sentence in a 500x100 box (32 px, 50 up), "Outline" (62 px,
// 100 down, black with a white 0.75 outline) and "Drop Shadow" (62 px, 200 down,
// with a red shadow offset (0.25, -0.25)).
//
// This is upstream's example BEFORE its rebuild into a game-over screen (#9566);
// re-port that one once text markup exists.
//
// DEVIATIONS:
// - no text markup: the rainbow sentence is the same text with its [color] tags
//   removed, so every word is white.
//
#include <memory>
#include <string>

#include "../exampleApp.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

class UiTextExample final: public ExampleApp
{
public:
    UiTextExample()
        : ExampleApp({.title = "UI Text", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

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
        _font = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/courier.json"));
        FontResource* font = nullptr;
        if (const auto fontRes = _font->resource();
            fontRes.has_value() && std::holds_alternative<FontResource*>(*fontRes)) {
            font = std::get<FontResource*>(*fontRes);
        }
        if (!font) {
            spdlog::error("Failed to load fonts/courier.json");
            return false;
        }

        // Create a camera
        auto* cameraEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        if (auto* camera = cameraEntity->findComponent<CameraComponent>()) {
            camera->camera()->setClearColor(Color(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f));
        }

        // Create a 2D screen
        auto* screenEntity = new Entity();
        screenEntity->setEngine(engine());
        auto* screen = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
        screen->setReferenceResolution(Vector2(1280.0f, 720.0f));
        screen->setScaleBlend(0.5f);
        screen->setScreenSpace(true);
        screen->setScaleMode(ScreenScaleMode::Blend);
        root()->addChild(screenEntity);

        // Basic Text
        createText(screenEntity, font, "Basic Text", 42, 400.0f, 42.0f, false,
            Color(1.0f, 1.0f, 1.0f, 1.0f), 200.0f);

        // Markup Text with wrap
        createText(screenEntity, font,
            "There are seven colors in the rainbow: red, orange, yellow, green, blue, indigo and violet.",
            32, 500.0f, 100.0f, true, Color(1.0f, 1.0f, 1.0f, 1.0f), 50.0f);

        // Text with outline
        auto* outline = createText(screenEntity, font, "Outline", 62, 400.0f, 62.0f, false,
            Color(0.0f, 0.0f, 0.0f, 1.0f), -100.0f);
        outline->setOutlineColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        outline->setOutlineThickness(0.75f);

        // Text with drop shadow
        auto* dropShadow = createText(screenEntity, font, "Drop Shadow", 62, 600.0f, 62.0f, false,
            Color(1.0f, 1.0f, 1.0f, 1.0f), -200.0f);
        dropShadow->setShadowColor(Color(1.0f, 0.0f, 0.0f, 1.0f));
        dropShadow->setShadowOffset(Vector2(0.25f, -0.25f));

        return true;
    }

private:
    ElementComponent* createText(Entity* screenEntity, FontResource* font, const std::string& text,
        int fontSize, float width, float height, bool wrapLines, const Color& color, float offsetY) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        entity->setLocalPosition(0.0f, offsetY, 0.0f);
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        element->setup({.type = ElementType::Text,
            .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
            .pivot = Vector2(0.5f, 0.5f),
            .width = width,
            .height = height});
        element->setFontResource(font);
        element->setFontSize(fontSize);
        element->setText(text);
        element->setWrapLines(wrapLines);
        element->setHorizontalAlign(ElementHorizontalAlign::Center);
        element->setColor(color);
        screenEntity->addChild(entity);
        return element;
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _font;
};

VISUTWIN_EXAMPLE_MAIN(UiTextExample)
