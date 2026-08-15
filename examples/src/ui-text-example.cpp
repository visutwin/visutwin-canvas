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
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "framework/engine.h"
#include "log.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

using namespace visutwin::canvas;

namespace
{
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    const std::string rootPath = ASSET_DIR;

    // Bitmap/MSDF font; the FONT handler also loads the sibling courier.png atlas.
    const auto courierFont = std::make_unique<Asset>(
        "courier-font",
        AssetType::FONT,
        rootPath + "/fonts/courier.json"
    );

    // Creates a Text ElementComponent parented under the screen entity.
    // Positioning is in UI pixels via the entity's local position (the text
    // render path reads element->entity()->position()); anchor/pivot describe
    // the element's own alignment.
    ElementComponent* createTextElement(
        Engine* engine,
        Entity* screenEntity,
        const std::string& text,
        int fontSize,
        const Color& color,
        const Vector2& pivot,
        const Vector4& anchor,
        float width,
        FontResource* font)
    {
        auto* entity = new Entity();
        entity->setEngine(engine);

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

    Entity* createBox(Engine* engine, StandardMaterial* material)
    {
        auto* entity = new Entity();
        entity->setEngine(engine);
        entity->setLocalScale(Vector3(1.4f, 1.4f, 1.4f));

        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        if (render) {
            render->setType("box");
            render->setMaterial(material);
            render->setLayers({LAYERID_WORLD});
        }

        engine->root()->addChild(entity);
        return entity;
    }
}

int main()
{
    log::init();
    log::set_level_debug();

    const auto shutdown = []() {
        if (renderer) {
            SDL_DestroyRenderer(renderer);
            renderer = nullptr;
        }
        if (window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        SDL_Quit();
    };

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "UI Text (Screen + Element)",
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        shutdown();
        return -1;
    }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        shutdown();
        return -1;
    }

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) {
        shutdown();
        return -1;
    }

    auto elementInput = std::make_shared<ElementInput>();

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ScreenComponentSystem>();
    createOptions.registerComponentSystem<ElementComponentSystem>();
    createOptions.elementInput = elementInput;

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setAmbientLight(0.35f, 0.35f, 0.4f);

    // --- Simple 3D backdrop: a single rotating box. ---
    auto boxMaterial = std::make_shared<StandardMaterial>();
    boxMaterial->setDiffuse(Color(0.35f, 0.55f, 0.85f, 1.0f));
    auto* boxEntity = createBox(engine.get(), boxMaterial.get());

    auto* lightEntity = new Entity();
    lightEntity->setEngine(engine.get());
    if (auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>())) {
        light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        light->setIntensity(1.0f);
    }
    lightEntity->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
    engine->root()->addChild(lightEntity);

    // --- Main perspective camera renders the 3D world (not the UI layer). ---
    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    if (cameraComponent && cameraComponent->camera()) {
        cameraComponent->camera()->setClearColor(Color(18.0f / 255.0f, 20.0f / 255.0f, 28.0f / 255.0f, 1.0f));
    }
    cameraEntity->setLocalPosition(0.0f, 0.0f, 5.0f);
    cameraComponent->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX});
    engine->root()->addChild(cameraEntity);

    // --- Orthographic UI camera renders only the UI layer, over the world. ---
    auto* uiCameraEntity = new Entity();
    uiCameraEntity->setEngine(engine.get());
    auto* uiCamera = static_cast<CameraComponent*>(uiCameraEntity->addComponent<CameraComponent>());
    if (uiCamera && uiCamera->camera()) {
        uiCamera->camera()->setProjection(ProjectionType::Orthographic);
        uiCamera->camera()->setOrthoHeight(static_cast<float>(WINDOW_HEIGHT) * 0.5f);
        uiCamera->camera()->setClearColorBuffer(false);
        uiCamera->camera()->setClearDepthBuffer(true);
        uiCamera->camera()->setClearStencilBuffer(true);
        uiCamera->setLayers({LAYERID_UI});
    }
    uiCameraEntity->setLocalPosition(0.0f, 0.0f, 10.0f);
    engine->root()->addChild(uiCameraEntity);

    // --- Screen-space Screen (UI root). ---
    auto* screenEntity = new Entity();
    screenEntity->setEngine(engine.get());
    auto* screenComponent = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
    if (screenComponent) {
        screenComponent->setReferenceResolution(Vector2(1280.0f, 720.0f));
        screenComponent->setScreenSpace(true);
    }
    engine->root()->addChild(screenEntity);

    // --- Load the bitmap font used by every text element. ---
    FontResource* fontResource = nullptr;
    if (const auto fontRes = courierFont->resource(); fontRes.has_value() && std::holds_alternative<FontResource*>(*fontRes)) {
        fontResource = std::get<FontResource*>(*fontRes);
    }
    if (!fontResource) {
        spdlog::warn("Courier bitmap font was not loaded; text elements will render nothing.");
    }

    // Large title (top-center), a subtitle, two colored size samples and a
    // live-updating line. All are Text ElementComponents; the placement is done
    // in UI pixels each frame so they track window resizes.
    auto* titleElement = createTextElement(
        engine.get(), screenEntity, "Canvas", 48,
        Color(1.0f, 1.0f, 1.0f, 1.0f), Vector2(0.5f, 0.5f),
        Vector4(0.5f, 1.0f, 0.5f, 1.0f), 600.0f, fontResource);

    auto* subtitleElement = createTextElement(
        engine.get(), screenEntity, "Screen-space UI text via Screen + Element", 22,
        Color(0.7f, 0.8f, 1.0f, 1.0f), Vector2(0.5f, 0.5f),
        Vector4(0.5f, 1.0f, 0.5f, 1.0f), 600.0f, fontResource);

    auto* warmElement = createTextElement(
        engine.get(), screenEntity, "Warm 28px", 28,
        Color(1.0f, 0.65f, 0.2f, 1.0f), Vector2(0.5f, 0.5f),
        Vector4(0.5f, 0.5f, 0.5f, 0.5f), 400.0f, fontResource);

    auto* coolElement = createTextElement(
        engine.get(), screenEntity, "Cool 18px", 18,
        Color(0.4f, 1.0f, 0.7f, 1.0f), Vector2(0.5f, 0.5f),
        Vector4(0.5f, 0.5f, 0.5f, 0.5f), 400.0f, fontResource);

    auto* liveElement = createTextElement(
        engine.get(), screenEntity, "frame 0", 20,
        Color(1.0f, 1.0f, 0.4f, 1.0f), Vector2(0.5f, 0.5f),
        Vector4(0.5f, 0.0f, 0.5f, 0.0f), 500.0f, fontResource);

    spdlog::info("UI Text example: a screen-space Screen with 5 Text ElementComponents "
                 "(48px title, 22px subtitle, warm 28px, cool 18px, and a live frame/FPS counter) "
                 "over a rotating 3D box. ESC to quit.");

    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    uint64_t frame = 0;
    float fpsSmoothed = 0.0f;
    float boxAngle = 0.0f;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(nowCounter - prevCounter) / static_cast<float>(perfFreq);
        prevCounter = nowCounter;

        const float instFps = dt > 1e-6f ? 1.0f / dt : 0.0f;
        fpsSmoothed = fpsSmoothed <= 0.0f ? instFps : (fpsSmoothed * 0.92f + instFps * 0.08f);

        // Compute the UI-space extent the same way the text render path does.
        int windowW = 1;
        int windowH = 1;
        SDL_GetWindowSize(window, &windowW, &windowH);
        float uiWidth = static_cast<float>(windowW);
        float uiHeight = static_cast<float>(windowH);
        if (screenComponent) {
            screenComponent->updateScaleFromWindow(windowW, windowH);
            const float scale = std::max(screenComponent->scale(), 1e-6f);
            uiWidth = screenComponent->resolution().x / scale;
            uiHeight = screenComponent->resolution().y / scale;
        }
        if (uiCamera && uiCamera->camera()) {
            uiCamera->camera()->setOrthoHeight(uiHeight * 0.5f);
        }

        // Position each element (UI pixels, origin top-left) so it stays anchored.
        const float cx = uiWidth * 0.5f;
        if (titleElement && titleElement->entity()) {
            titleElement->entity()->setLocalPosition(cx, 70.0f, 0.0f);
        }
        if (subtitleElement && subtitleElement->entity()) {
            subtitleElement->entity()->setLocalPosition(cx, 118.0f, 0.0f);
        }
        if (warmElement && warmElement->entity()) {
            warmElement->entity()->setLocalPosition(cx, uiHeight * 0.5f - 20.0f, 0.0f);
        }
        if (coolElement && coolElement->entity()) {
            coolElement->entity()->setLocalPosition(cx, uiHeight * 0.5f + 20.0f, 0.0f);
        }
        if (liveElement && liveElement->entity()) {
            liveElement->entity()->setLocalPosition(cx, uiHeight - 60.0f, 0.0f);
        }

        // Live-updating text: rewrite the counter line every frame.
        if (liveElement) {
            char buffer[96];
            std::snprintf(buffer, sizeof(buffer), "frame %llu   |   %.0f FPS",
                static_cast<unsigned long long>(frame), fpsSmoothed);
            liveElement->setText(buffer);
        }

        // Spin the backdrop box.
        boxAngle += dt * 40.0f;
        if (boxAngle > 360.0f) {
            boxAngle -= 360.0f;
        }
        if (boxEntity) {
            boxEntity->setLocalEulerAngles(boxAngle * 0.6f, boxAngle, 0.0f);
        }

        engine->update(dt);

        // Rebuild/refresh the glyph meshes for all text elements, then present.
        elementInput->syncTextElements();
        engine->render();

        ++frame;
    }

    shutdown();
    return 0;
}
