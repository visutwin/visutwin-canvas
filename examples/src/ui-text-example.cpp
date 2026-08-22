// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Screen-space UI text example.
//
// Mirrors the upstream `user-interface/text` example: a screen-space
// Screen root hosts several Text ElementComponents at different positions,
// font sizes and colors. One line updates every frame (frame counter + FPS)
// via ElementComponent::setText to demonstrate live text.
//
// The on-screen text render path is the same one proven by
// world-to-screen-example.cpp:
//   * A ScreenComponent (screen-space) root entity.
//   * Text ElementComponents parented under the screen, positioned in UI
//     pixels, each pointed at a loaded bitmap font (FontResource).
//   * ElementInput::syncTextElements() turns each Text element into a glyph
//     mesh on LAYERID_UI, which an orthographic UI camera composites over the
//     3D backdrop.
//
#include <algorithm>
#include <cstdio>
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
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

class UiTextExample final: public ExampleApp
{
public:
    UiTextExample()
        : ExampleApp({.title = "UI Text (Screen + Element)",
                      .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

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
        scene()->setAmbientLight(0.35f, 0.35f, 0.4f);

        // --- Simple 3D backdrop: a single rotating box. ---
        _boxMaterial = std::make_shared<StandardMaterial>();
        _boxMaterial->setDiffuse(Color(0.35f, 0.55f, 0.85f, 1.0f));
        _boxEntity = createPrimitive("box", _boxMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(1.4f, 1.4f, 1.4f), {LAYERID_WORLD});

        createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f));

        // --- Main perspective camera renders the 3D world (not the UI layer). ---
        auto* cameraEntity = createCamera(Vector3(0.0f, 0.0f, 5.0f));
        auto* cameraComponent = cameraEntity->findComponent<CameraComponent>();
        if (cameraComponent && cameraComponent->camera()) {
            cameraComponent->camera()->setClearColor(
                Color(18.0f / 255.0f, 20.0f / 255.0f, 28.0f / 255.0f, 1.0f));
        }
        cameraComponent->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX});

        // --- Orthographic UI camera renders only the UI layer, over the world. ---
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

        // --- Screen-space Screen (UI root). ---
        auto* screenEntity = new Entity();
        screenEntity->setEngine(engine());
        _screenComponent = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
        if (_screenComponent) {
            _screenComponent->setReferenceResolution(Vector2(1280.0f, 720.0f));
            _screenComponent->setScreenSpace(true);
        }
        root()->addChild(screenEntity);

        // --- Load the bitmap font used by every text element. ---
        // Bitmap/MSDF font; the FONT handler also loads the sibling courier.png atlas.
        _courierFont = std::make_unique<Asset>(
            "courier-font", AssetType::FONT, assetPath("fonts/courier.json"));

        FontResource* fontResource = nullptr;
        if (const auto fontRes = _courierFont->resource();
            fontRes.has_value() && std::holds_alternative<FontResource*>(*fontRes)) {
            fontResource = std::get<FontResource*>(*fontRes);
        }
        if (!fontResource) {
            spdlog::warn("Courier bitmap font was not loaded; text elements will render nothing.");
        }

        // Large title (top-center), a subtitle, two colored size samples and a
        // live-updating line. All are Text ElementComponents; the placement is done
        // in UI pixels each frame so they track window resizes.
        _titleElement = createTextElement(
            screenEntity, "Canvas", 48,
            Color(1.0f, 1.0f, 1.0f, 1.0f), Vector2(0.5f, 0.5f),
            Vector4(0.5f, 1.0f, 0.5f, 1.0f), 600.0f, fontResource);

        _subtitleElement = createTextElement(
            screenEntity, "Screen-space UI text via Screen + Element", 22,
            Color(0.7f, 0.8f, 1.0f, 1.0f), Vector2(0.5f, 0.5f),
            Vector4(0.5f, 1.0f, 0.5f, 1.0f), 600.0f, fontResource);

        _warmElement = createTextElement(
            screenEntity, "Warm 28px", 28,
            Color(1.0f, 0.65f, 0.2f, 1.0f), Vector2(0.5f, 0.5f),
            Vector4(0.5f, 0.5f, 0.5f, 0.5f), 400.0f, fontResource);

        _coolElement = createTextElement(
            screenEntity, "Cool 18px", 18,
            Color(0.4f, 1.0f, 0.7f, 1.0f), Vector2(0.5f, 0.5f),
            Vector4(0.5f, 0.5f, 0.5f, 0.5f), 400.0f, fontResource);

        _liveElement = createTextElement(
            screenEntity, "frame 0", 20,
            Color(1.0f, 1.0f, 0.4f, 1.0f), Vector2(0.5f, 0.5f),
            Vector4(0.5f, 0.0f, 0.5f, 0.0f), 500.0f, fontResource);

        spdlog::info("UI Text example: a screen-space Screen with 5 Text ElementComponents "
                     "(48px title, 22px subtitle, warm 28px, cool 18px, and a live frame/FPS counter) "
                     "over a rotating 3D box. ESC to quit.");

        return true;
    }

    void update(const float dt) override
    {
        const float instFps = dt > 1e-6f ? 1.0f / dt : 0.0f;
        _fpsSmoothed = _fpsSmoothed <= 0.0f ? instFps : (_fpsSmoothed * 0.92f + instFps * 0.08f);

        // Compute the UI-space extent the same way the text render path does.
        int windowW = 1;
        int windowH = 1;
        SDL_GetWindowSize(window(), &windowW, &windowH);
        float uiWidth = static_cast<float>(windowW);
        float uiHeight = static_cast<float>(windowH);
        if (_screenComponent) {
            _screenComponent->updateScaleFromWindow(windowW, windowH);
            const float scale = std::max(_screenComponent->scale(), 1e-6f);
            uiWidth = _screenComponent->resolution().x / scale;
            uiHeight = _screenComponent->resolution().y / scale;
        }
        if (_uiCamera && _uiCamera->camera()) {
            _uiCamera->camera()->setOrthoHeight(uiHeight * 0.5f);
        }

        // Position each element (UI pixels, origin top-left) so it stays anchored.
        const float cx = uiWidth * 0.5f;
        if (_titleElement && _titleElement->entity()) {
            _titleElement->entity()->setLocalPosition(cx, 70.0f, 0.0f);
        }
        if (_subtitleElement && _subtitleElement->entity()) {
            _subtitleElement->entity()->setLocalPosition(cx, 118.0f, 0.0f);
        }
        if (_warmElement && _warmElement->entity()) {
            _warmElement->entity()->setLocalPosition(cx, uiHeight * 0.5f - 20.0f, 0.0f);
        }
        if (_coolElement && _coolElement->entity()) {
            _coolElement->entity()->setLocalPosition(cx, uiHeight * 0.5f + 20.0f, 0.0f);
        }
        if (_liveElement && _liveElement->entity()) {
            _liveElement->entity()->setLocalPosition(cx, uiHeight - 60.0f, 0.0f);
        }

        // Live-updating text: rewrite the counter line every frame.
        if (_liveElement) {
            char buffer[96];
            std::snprintf(buffer, sizeof(buffer), "frame %llu   |   %.0f FPS",
                static_cast<unsigned long long>(_frame), _fpsSmoothed);
            _liveElement->setText(buffer);
        }

        // Spin the backdrop box.
        _boxAngle += dt * 40.0f;
        if (_boxAngle > 360.0f) {
            _boxAngle -= 360.0f;
        }
        if (_boxEntity) {
            _boxEntity->setLocalEulerAngles(_boxAngle * 0.6f, _boxAngle, 0.0f);
        }
    }

    void preRender() override
    {
        // Rebuild/refresh the glyph meshes for all text elements, then present.
        _elementInput->syncTextElements();
    }

    void postRender() override
    {
        ++_frame;
    }

private:
    // Creates a Text ElementComponent parented under the screen entity.
    // Positioning is in UI pixels via the entity's local position (the text
    // render path reads element->entity()->position()); anchor/pivot describe
    // the element's own alignment.
    ElementComponent* createTextElement(
        Entity* screenEntity,
        const std::string& text,
        int fontSize,
        const Color& color,
        const Vector2& pivot,
        const Vector4& anchor,
        float width,
        FontResource* font) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());

        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        if (element) {
            element->setType(ElementType::Text);
            element->setPivot(pivot);
            element->setAnchor(anchor);
            element->setWidth(width);
            element->setHeight(static_cast<float>(fontSize) + 6.0f);
            element->setFontSize(fontSize);
            element->setColor(color);
            element->setText(text);
            element->setHorizontalAlign(ElementHorizontalAlign::Center);
            element->setWrapLines(false);
            if (font) {
                element->setFontResource(font);
            }
        }

        screenEntity->addChild(entity);
        return element;
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _courierFont;
    std::shared_ptr<StandardMaterial> _boxMaterial;

    Entity* _boxEntity = nullptr;
    CameraComponent* _uiCamera = nullptr;
    ScreenComponent* _screenComponent = nullptr;

    ElementComponent* _titleElement = nullptr;
    ElementComponent* _subtitleElement = nullptr;
    ElementComponent* _warmElement = nullptr;
    ElementComponent* _coolElement = nullptr;
    ElementComponent* _liveElement = nullptr;

    uint64_t _frame = 0;
    float _fpsSmoothed = 0.0f;
    float _boxAngle = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(UiTextExample)
