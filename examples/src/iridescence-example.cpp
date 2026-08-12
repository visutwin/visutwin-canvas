// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Thin-film iridescence demo (KHR_materials_iridescence): the soap-bubble /
// oil-film color shift produced by thin-film interference. The interference
// color depends on both the view angle (Fresnel) and the film thickness, so a
// smooth, low-metalness, high-gloss surface under an environment map shows a
// rainbow sheen that sweeps as the camera orbits.
//
// Two rows of procedural spheres over a reflective floor:
//   Row 1 (top, y = +1.6): iridescence INTENSITY ramps 0 -> 1 left to right at a
//      fixed film thickness. The leftmost sphere is plain PBR; the rainbow sheen
//      strengthens across the row.
//   Row 2 (bottom, y = -1.6): full intensity, film THICKNESS (max) sweeps
//      100 nm -> 800 nm left to right. Different optical path lengths land on
//      different interference orders, so each sphere settles on a different hue
//      (blue/gold/magenta/green/...).
//
// Orbit the camera (LMB/RMB orbit, wheel zoom, F focus, R reset) to watch the
// hues shift with view angle.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/constants.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1000;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

const auto helipad = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

Entity* createSphere(Engine* engine, Material* material, const Vector3& position)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(1.35f, 1.35f, 1.35f);
    if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
        render->setMaterial(material);
        render->setType("sphere");
    }
    return entity;
}

int main()
{
    log::init();
    log::set_level_debug();

    window = nullptr;
    renderer = nullptr;

    const auto shutdown = []() {
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        SDL_Quit();
    };

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Thin-Film Iridescence", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) { shutdown(); return -1; }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { shutdown(); return -1; }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) { shutdown(); return -1; }

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) { shutdown(); return -1; }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.18f, 0.18f, 0.2f);
    scene->setExposure(1.0f);

    // Environment atlas drives the reflections that make the thin-film sheen visible.
    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad env-atlas");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));
    scene->setSkyboxMip(1);
    scene->setSkyboxIntensity(0.5f);

    // Directional key light.
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lightComponent->setIntensity(1.5f);
    }
    light->setLocalEulerAngles(45.0f, -30.0f, 0.0f);
    engine->root()->addChild(light);

    // Reflective floor to catch some of the environment.
    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setDiffuse(Color(0.05f, 0.05f, 0.06f, 1.0f));
    floorMaterial->setMetalness(0.0f);
    floorMaterial->setGlossInvert(false);
    floorMaterial->setGloss(0.85f);
    {
        auto* floor = new Entity();
        floor->setEngine(engine.get());
        floor->setLocalPosition(0.0f, -3.2f, 0.0f);
        floor->setLocalScale(40.0f, 1.0f, 40.0f);
        if (auto* render = static_cast<RenderComponent*>(floor->addComponent<RenderComponent>())) {
            render->setMaterial(floorMaterial.get());
            render->setType("plane");
        }
        engine->root()->addChild(floor);
    }

    // Iridescence looks best on a smooth, near-dielectric surface with strong
    // environment reflections: metalness 0, high gloss.
    constexpr int COLUMNS = 6;
    const float spacing = 3.1f;
    const float xStart = -0.5f * spacing * static_cast<float>(COLUMNS - 1);

    // Keep the material shared_ptrs alive for the app lifetime.
    std::vector<std::shared_ptr<StandardMaterial>> materials;

    const auto makeIridescent = [&](const float intensity, const float thicknessMin,
                                    const float thicknessMax, const float ior) {
        auto m = std::make_shared<StandardMaterial>();
        m->setDiffuse(Color(0.9f, 0.9f, 0.92f, 1.0f));   // light dielectric base
        m->setMetalness(0.0f);
        m->setGlossInvert(false);
        m->setGloss(0.92f);                              // smooth -> crisp reflections
        m->setIridescenceIntensity(intensity);
        m->setIridescenceIOR(ior);
        m->setIridescenceThicknessMin(thicknessMin);
        m->setIridescenceThicknessMax(thicknessMax);
        materials.push_back(m);
        return m;
    };

    // Row 1: intensity ramp 0 -> 1, fixed film thickness (~400 nm), IOR 1.3.
    for (int c = 0; c < COLUMNS; ++c) {
        const float t = static_cast<float>(c) / static_cast<float>(COLUMNS - 1);
        const float intensity = t;                        // 0.0 .. 1.0
        auto m = makeIridescent(intensity, 300.0f, 400.0f, 1.3f);
        const float x = xStart + spacing * static_cast<float>(c);
        engine->root()->addChild(createSphere(engine.get(), m.get(), Vector3(x, 1.6f, 0.0f)));
    }

    // Row 2: full intensity, film thickness (max) sweep 100 -> 800 nm, IOR 1.3.
    constexpr float THICKNESS_MIN_NM = 100.0f;
    constexpr float THICKNESS_MAX_NM = 800.0f;
    for (int c = 0; c < COLUMNS; ++c) {
        const float t = static_cast<float>(c) / static_cast<float>(COLUMNS - 1);
        const float thickness = THICKNESS_MIN_NM + t * (THICKNESS_MAX_NM - THICKNESS_MIN_NM);
        // Thin uniform film: min == max so the whole sphere shares one interference order.
        auto m = makeIridescent(1.0f, thickness, thickness, 1.3f);
        const float x = xStart + spacing * static_cast<float>(c);
        engine->root()->addChild(createSphere(engine.get(), m.get(), Vector3(x, -1.6f, 0.0f)));
    }

    // Orbit camera framed on the grid center.
    const auto start = Vector3(0.0f, 0.0f, 24.0f);
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();
    camera->setPosition(start);
    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    const Vector3 focus(0.0f, 0.0f, 0.0f);
    cameraControls->setFocusPoint(focus);
    cameraControls->setEnableFly(false);
    cameraControls->setMoveSpeed(20.0f);
    cameraControls->setMoveFastSpeed(40.0f);
    cameraControls->setMoveSlowSpeed(8.0f);
    cameraControls->setOrbitDistance(24.0f);
    cameraControls->storeResetState();

    if (cameraComponent) {
        auto taa = cameraComponent->taa();
        taa.enabled = false;
        cameraComponent->setTaa(taa);
    }

    spdlog::info("Thin-film iridescence: row 1 = intensity 0->1, row 2 = film thickness {}->{} nm at full intensity",
        THICKNESS_MIN_NM, THICKNESS_MAX_NM);
    spdlog::info("Orbit: LMB/RMB orbit, wheel zoom, F focus, R reset, Esc quit");

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(focus, 24.0f);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            } else if (event.type == SDL_EVENT_PINCH_UPDATE && cameraControls) {
                const float pinchDelta = (event.pinch.scale - 1.0f) * 10.0f;
                cameraControls->addZoomInput(pinchDelta);
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
