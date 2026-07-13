// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Local light PCSS demo: a spot light (left pillar group) and an omni light
// (right pillar group) cast contact-hardening soft shadows (upstream
// shadowPCSS.js port). Auto-cycles PCF <-> PCSS on both lights every 3 s —
// with PCSS the shadows sharpen at the pillar base and soften with distance.
// Keys: 1 = PCF, 2 = PCSS, Space = auto-cycle, Esc = quit.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>

#include <QuartzCore/QuartzCore.hpp>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

Entity* createEntity(Engine* engine, Material* material, const char* type,
    const Vector3& position, const Vector3& scale, const bool castShadows)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
    if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
        render->setMaterial(material);
        render->setType(type);
        render->setCastShadows(castShadows);
        render->setReceiveShadows(true);
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
        "VisuTwin Local PCSS Shadows", WINDOW_WIDTH, WINDOW_HEIGHT,
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

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.08f, 0.08f, 0.1f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->setLocalPosition(0.0f, 7.0f, 14.0f);
    camera->setLocalEulerAngles(-26.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    // Receiver-only floor (large ground as caster would inflate depth fits).
    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setDiffuse(Color(0.55f, 0.55f, 0.58f, 1.0f));
    floorMaterial->setGlossInvert(true);
    floorMaterial->setGloss(0.85f);
    engine->root()->addChild(createEntity(
        engine.get(), floorMaterial.get(), "plane", Vector3(0.0f, 0.0f, 0.0f), Vector3(40.0f, 1.0f, 40.0f), false));

    // Pillars: tall boxes so shadow softness visibly grows with distance.
    auto pillarMaterial = std::make_shared<StandardMaterial>();
    pillarMaterial->setDiffuse(Color(0.7f, 0.5f, 0.35f, 1.0f));
    pillarMaterial->setGlossInvert(true);
    pillarMaterial->setGloss(0.7f);
    // Left group (spot light).
    engine->root()->addChild(createEntity(
        engine.get(), pillarMaterial.get(), "box", Vector3(-4.5f, 1.5f, 0.0f), Vector3(0.6f, 3.0f, 0.6f), true));
    engine->root()->addChild(createEntity(
        engine.get(), pillarMaterial.get(), "box", Vector3(-2.5f, 0.9f, -1.5f), Vector3(0.5f, 1.8f, 0.5f), true));
    // Right group (omni light).
    engine->root()->addChild(createEntity(
        engine.get(), pillarMaterial.get(), "box", Vector3(4.5f, 1.5f, 0.0f), Vector3(0.6f, 3.0f, 0.6f), true));
    engine->root()->addChild(createEntity(
        engine.get(), pillarMaterial.get(), "sphere", Vector3(2.6f, 0.7f, -1.5f), Vector3(1.4f, 1.4f, 1.4f), true));

    // Spot light over the left group.
    auto* spotLight = new Entity();
    spotLight->setEngine(engine.get());
    auto* spot = static_cast<LightComponent*>(spotLight->addComponent<LightComponent>());
    if (spot) {
        spot->setType(LightType::LIGHTTYPE_SPOT);
        spot->setColor(Color(1.0f, 0.95f, 0.85f, 1.0f));
        spot->setIntensity(2.6f);
        spot->setRange(30.0f);
        spot->setInnerConeAngle(35.0f);
        spot->setOuterConeAngle(55.0f);
        spot->setCastShadows(true);
        spot->setShadowResolution(1024);
        spot->setShadowBias(0.0005f);
        spot->setShadowNormalBias(0.02f);
        spot->setPenumbraSize(30.0f);  // local-light scale: search px on the shadow map
    }
    // Emission is the node's -Y axis (straight down). Hover slightly behind the
    // pillars so their shadows stretch toward the camera INSIDE the lit pool.
    spotLight->setLocalPosition(-3.6f, 8.0f, -2.0f);
    engine->root()->addChild(spotLight);

    // Omni light over the right group.
    auto* omniLight = new Entity();
    omniLight->setEngine(engine.get());
    auto* omni = static_cast<LightComponent*>(omniLight->addComponent<LightComponent>());
    if (omni) {
        omni->setType(LightType::LIGHTTYPE_OMNI);
        omni->setColor(Color(0.7f, 0.85f, 1.0f, 1.0f));
        omni->setIntensity(2.2f);
        omni->setRange(25.0f);
        omni->setCastShadows(true);
        omni->setShadowResolution(1024);
        omni->setShadowBias(0.0005f);
        omni->setPenumbraSize(30.0f);
    }
    omniLight->setLocalPosition(4.0f, 6.0f, 2.0f);
    engine->root()->addChild(omniLight);

    spdlog::info("Local PCSS: spot (left, warm) + omni (right, cool), PCF <-> PCSS every 3 s");
    spdlog::info("Keys: 1 = PCF, 2 = PCSS, Space = auto-cycle, Esc = quit");

    const auto applyMode = [&](const bool pcss) {
        if (spot) spot->setShadowType(pcss ? SHADOW_PCSS_32F : SHADOW_PCF3_32F);
        if (omni) omni->setShadowType(pcss ? SHADOW_PCSS_32F : SHADOW_PCF3_32F);
        spdlog::info("Local shadow mode: {}", pcss ? "PCSS (contact-hardening)" : "PCF 3x3");
    };
    applyMode(false);

    bool running = true;
    bool usePcss = false;
    bool autoCycle = true;
    float cycleTimer = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_1) {
                autoCycle = false;
                usePcss = false;
                applyMode(false);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_2) {
                autoCycle = false;
                usePcss = true;
                applyMode(true);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                autoCycle = true;
                cycleTimer = 0.0f;
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (autoCycle) {
            cycleTimer += static_cast<float>(dtSeconds);
            if (cycleTimer >= 3.0f) {
                cycleTimer = 0.0f;
                usePcss = !usePcss;
                applyMode(usePcss);
            }
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
