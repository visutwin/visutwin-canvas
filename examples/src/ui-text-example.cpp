// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/text.
//
// A screen-space Screen (reference resolution 1280x720) over a dark grey clear
// holds four Text elements in the courier bitmap font, centred horizontally
// and placed relative to the screen centre: "Basic Text" (42 px, 200 up), a
// wrapped rainbow sentence in a 500x100 box (32 px, 50 up), "Outline" (62 px,
// 100 down) and "Drop Shadow" (62 px, 200 down).
//
// DEVIATIONS:
// - the element system has no anchors, so each element's entity position is
//   set in UI pixels (top-left origin) every frame, from the screen's size.
// - UI text is drawn by a separate orthographic camera on LAYERID_UI; the main
//   camera renders only the world layers, which are empty here.
// - ScreenComponent has no scaleMode / scaleBlend: it always scales by the
//   smaller of the two axis ratios. At the reference resolution both agree.
// - no text markup: the rainbow sentence is the same text with its [color] tags
//   removed, so every word is white.
// - no outline: "Outline" is black with a white 0.75 outline upstream, which
//   without the outline is invisible on the dark clear, so it is drawn in the
//   outline colour (white) instead.
// - no drop shadow: "Drop Shadow" is drawn without its red (0.25, -0.25) shadow.
//
#include <algorithm>
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
            camera->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX});
        }

        // Orthographic camera for the UI layer, drawn over the main camera.
        auto* uiCameraEntity = createCamera(Vector3(0.0f, 0.0f, 10.0f));
        _uiCamera = uiCameraEntity->findComponent<CameraComponent>();
        if (_uiCamera && _uiCamera->camera()) {
            _uiCamera->camera()->setProjection(ProjectionType::Orthographic);
            _uiCamera->camera()->setOrthoHeight(static_cast<float>(WINDOW_HEIGHT) * 0.5f);
            _uiCamera->camera()->setClearColorBuffer(false);
            _uiCamera->camera()->setClearDepthBuffer(true);
            _uiCamera->camera()->setClearStencilBuffer(true);
            _uiCamera->setLayers({LAYERID_UI});
        }

        // Create a 2D screen
        auto* screenEntity = new Entity();
        screenEntity->setEngine(engine());
        _screen = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
        _screen->setReferenceResolution(Vector2(1280.0f, 720.0f));
        _screen->setScreenSpace(true);
        root()->addChild(screenEntity);

        // Basic Text
        _texts.push_back({createText(screenEntity, font, "Basic Text", 42, 400.0f, 42.0f, false,
            Color(1.0f, 1.0f, 1.0f, 1.0f)), 200.0f});

        // Markup Text with wrap
        _texts.push_back({createText(screenEntity, font,
            "There are seven colors in the rainbow: red, orange, yellow, green, blue, indigo and violet.",
            32, 500.0f, 100.0f, true, Color(1.0f, 1.0f, 1.0f, 1.0f)), 50.0f});

        // Text with outline
        _texts.push_back({createText(screenEntity, font, "Outline", 62, 400.0f, 62.0f, false,
            Color(1.0f, 1.0f, 1.0f, 1.0f)), -100.0f});

        // Text with drop shadow
        _texts.push_back({createText(screenEntity, font, "Drop Shadow", 62, 600.0f, 62.0f, false,
            Color(1.0f, 1.0f, 1.0f, 1.0f)), -200.0f});

        return true;
    }

    void update(float) override
    {
        int windowW = 1;
        int windowH = 1;
        SDL_GetWindowSize(window(), &windowW, &windowH);
        _screen->updateScaleFromWindow(windowW, windowH);
        const float scale = std::max(_screen->scale(), 1e-6f);
        const float uiWidth = _screen->resolution().x / scale;
        const float uiHeight = _screen->resolution().y / scale;
        if (_uiCamera && _uiCamera->camera()) {
            _uiCamera->camera()->setOrthoHeight(uiHeight * 0.5f);
        }

        // Upstream anchors every element at the screen centre and offsets it with a
        // y-up local position; this places the same point in y-down UI pixels.
        for (const auto& [element, offsetY] : _texts) {
            element->entity()->setLocalPosition(uiWidth * 0.5f, uiHeight * 0.5f - offsetY, 0.0f);
        }
    }

    void preRender() override
    {
        _elementInput->syncTextElements();
    }

private:
    struct PlacedText
    {
        ElementComponent* element = nullptr;
        float offsetY = 0.0f;
    };

    ElementComponent* createText(Entity* screenEntity, FontResource* font, const std::string& text,
        int fontSize, float width, float height, bool wrapLines, const Color& color) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        element->setType(ElementType::Text);
        element->setPivot(Vector2(0.5f, 0.5f));
        element->setAnchor(Vector4(0.5f, 0.5f, 0.5f, 0.5f));
        element->setFontResource(font);
        element->setFontSize(fontSize);
        element->setText(text);
        element->setWidth(width);
        element->setHeight(height);
        element->setWrapLines(wrapLines);
        element->setHorizontalAlign(ElementHorizontalAlign::Center);
        element->setColor(color);
        screenEntity->addChild(entity);
        return element;
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _font;

    CameraComponent* _uiCamera = nullptr;
    ScreenComponent* _screen = nullptr;
    std::vector<PlacedText> _texts;
};

VISUTWIN_EXAMPLE_MAIN(UiTextExample)
