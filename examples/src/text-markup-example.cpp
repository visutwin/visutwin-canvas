// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream test/text-markup (a hidden test example).
//
// A screen-space screen (reference 1280 x 720, blend 0.5) over a camera clearing to light
// grey (0.85). Roboto Regular text elements, centred on the screen and pivoted at their
// middles, size 36, render each style twice: with markup off in the left column (x -300)
// and on in the right one (x +300). The two sides must look identical; a colour difference
// points at the gamma handling of the markup colour path. The rows, top to bottom:
//   - the column titles "Markup OFF" / "Markup ON", black, size 48, at y 300;
//   - an element colour #454A53 with no tags, at y 200;
//   - a colour #803333, as the element colour on the left and as a [color] tag on the right,
//     at y 100;
//   - an outline #204080, thickness 0.6, on #B0B8C4 text, as element settings and as an
//     [outline] tag, at y 0;
//   - a shadow #663311, offset (0.25, -0.25), on #B0B8C4 text, as element settings and as a
//     [shadow] tag with offsetX / offsetY, at y -100;
//   - one centred size-30 line mixing colour, outline and shadow tags over #454A53, at
//     y -220.
//
// DEVIATION: glyphs of different markup styles are separate draws (upstream draws a page in
// one mesh with the style in vertex attributes); overlapping neighbours' outlines can order
// differently.
//
#include <memory>
#include <optional>
#include <string>

#include "../exampleApp.h"
#include "../uiElements.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kColumnX = 300.0f;

    Color hex(const std::string& value)
    {
        Color color;
        color.fromString(value);
        return color;
    }

    /// Outline and shadow settings of a text element, which ElementProps does not carry.
    struct TextStyle
    {
        std::optional<Color> outlineColor;
        float outlineThickness = 0.0f;
        std::optional<Color> shadowColor;
        Vector2 shadowOffset = Vector2(0.0f, 0.0f);
    };
}

class TextMarkupExample final: public ExampleApp
{
public:
    TextMarkupExample(): ExampleApp({.title = "Text Markup"}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _font = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        FontResource* font = _font->resourceAs<FontResource>();
        if (!font) {
            spdlog::error("Failed to load fonts/roboto-regular.json");
            return false;
        }

        // A camera.
        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.85f, 0.85f, 0.85f, 1.0f));

        // A 2D screen.
        _screen = createScreen();

        // What every text shares.
        _defaults = {
            .type = ElementType::Text,
            .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
            .pivot = Vector2(0.5f, 0.5f),
            .font = font,
            .fontSize = 36,
        };

        // Each row renders the same text twice: with markup disabled (left) and enabled
        // (right).
        createText(-kColumnX, 300.0f, {.color = hex("#000000"), .text = "Markup OFF", .fontSize = 48});
        createText(kColumnX, 300.0f, {.color = hex("#000000"), .text = "Markup ON", .fontSize = 48});

        // Element colour, no tags.
        createText(-kColumnX, 200.0f, {.color = hex("#454A53"), .text = "Element color #454A53"});
        createText(kColumnX, 200.0f, {.color = hex("#454A53"), .text = "Element color #454A53",
                                      .enableMarkup = true});

        // Colour tags.
        createText(-kColumnX, 100.0f, {.color = hex("#803333"), .text = "Color tag #803333"});
        createText(kColumnX, 100.0f, {.text = R"([color="#803333"]Color tag #803333[/color])",
                                      .enableMarkup = true});

        // Outline colour and thickness.
        createText(-kColumnX, 0.0f, {.color = hex("#B0B8C4"), .text = "Outline #204080"},
            {.outlineColor = hex("#204080"), .outlineThickness = 0.6f});
        createText(kColumnX, 0.0f, {.color = hex("#B0B8C4"),
                                    .text = R"([outline color="#204080" thickness="0.6"]Outline #204080[/outline])",
                                    .enableMarkup = true});

        // Shadow colour and offset.
        createText(-kColumnX, -100.0f, {.color = hex("#B0B8C4"), .text = "Shadow #663311"},
            {.shadowColor = hex("#663311"), .shadowOffset = Vector2(0.25f, -0.25f)});
        createText(kColumnX, -100.0f, {.color = hex("#B0B8C4"),
                                       .text = R"([shadow color="#663311" offsetX="0.25" offsetY="-0.25"]Shadow #663311[/shadow])",
                                       .enableMarkup = true});

        // Mixed tags in one string.
        createText(0.0f, -220.0f, {.color = hex("#454A53"),
                                   .text = R"(Mixed: [color="#803333"]red[/color] plain [color="#204080"]blue[/color] )"
                                           R"([outline color="#663311" thickness="0.5"]outlined[/outline] )"
                                           R"([shadow color="#803333" offsetX="0.2" offsetY="-0.2"]shadowed[/shadow])",
                                   .fontSize = 30, .enableMarkup = true});
        return true;
    }

private:
    /// A text element at (x, y) on the screen, from `props` over the shared defaults.
    void createText(const float x, const float y, const ElementProps& props, const TextStyle& style = {})
    {
        ElementComponent* element = createElement(engine(), _screen->entity(), props, _defaults);
        if (style.outlineColor) {
            element->setOutlineColor(*style.outlineColor);
            element->setOutlineThickness(style.outlineThickness);
        }
        if (style.shadowColor) {
            element->setShadowColor(*style.shadowColor);
            element->setShadowOffset(style.shadowOffset);
        }
        element->entity()->setLocalPosition(x, y, 0.0f);
    }

    std::unique_ptr<Asset> _font;
    ScreenComponent* _screen = nullptr;
    ElementProps _defaults;
};

VISUTWIN_EXAMPLE_MAIN(TextMarkupExample)
