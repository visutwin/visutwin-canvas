// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Screen-space reflections demo: a glossy floor (transparent material, drawn
// after the mid-frame scene-color + depth grab) ray-marches the reflection of
// the opaque objects standing on it against the scene depth grab, sampling the
// scene color grab at the hit. Off-screen rays fall back to the env atlas.
// Auto-toggles SSR on/off every 3 s — with SSR ON the objects appear reflected
// in the floor; OFF, the floor shows only the flat env reflection. Esc quits.
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
#include "platform/graphics/blendState.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

Entity* createEntity(Engine* engine, Material* material, const char* type,
    const Vector3& position, const Vector3& scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
    if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
        render->setMaterial(material);
        render->setType(type);
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
        "VisuTwin Screen-Space Reflections", WINDOW_WIDTH, WINDOW_HEIGHT,
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
    scene->setAmbientLight(0.14f, 0.15f, 0.18f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->setLocalPosition(0.0f, 2.4f, 7.5f);
    camera->setLocalEulerAngles(-16.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    // SSR needs both the mid-frame scene color grab and the depth grab.
    if (cameraComponent) {
        cameraComponent->requestSceneColorMap(true);
        cameraComponent->requestSceneDepthMap(true);
    }

    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 0.97f, 0.9f, 1.0f));
        lightComponent->setIntensity(1.6f);
    }
    light->setLocalEulerAngles(48.0f, -28.0f, 0.0f);
    engine->root()->addChild(light);

    // Opaque colored objects standing on the floor (the reflected content).
    struct ColMat { std::shared_ptr<StandardMaterial> mat; };
    std::vector<std::shared_ptr<StandardMaterial>> objMats;
    const Color colors[3] = {Color(0.9f, 0.25f, 0.2f, 1.0f), Color(0.25f, 0.7f, 0.3f, 1.0f), Color(0.3f, 0.45f, 0.95f, 1.0f)};
    const Vector3 positions[3] = {Vector3(-2.4f, 0.9f, 0.0f), Vector3(0.0f, 0.7f, -0.5f), Vector3(2.4f, 1.1f, 0.3f)};
    const char* types[3] = {"box", "sphere", "box"};
    const Vector3 scales[3] = {Vector3(1.4f, 1.8f, 1.4f), Vector3(1.6f, 1.6f, 1.6f), Vector3(1.2f, 2.2f, 1.2f)};
    for (int i = 0; i < 3; ++i) {
        auto m = std::make_shared<StandardMaterial>();
        m->setDiffuse(colors[i]);
        m->setGlossInvert(true);
        m->setGloss(0.5f);
        objMats.push_back(m);
        engine->root()->addChild(createEntity(engine.get(), m.get(), types[i], positions[i], scales[i]));
    }

    // Glossy reflective floor: transparent material so it draws AFTER the grab
    // and can sample the opaque scene. SSR reflects the objects; env atlas fills
    // off-screen rays.
    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setDiffuse(Color(0.1f, 0.1f, 0.12f, 1.0f));
    floorMaterial->setMetalness(0.85f);
    floorMaterial->setGlossInvert(true);
    floorMaterial->setGloss(0.06f);   // sharp reflections
    floorMaterial->setUseScreenSpaceReflection(true);
    floorMaterial->setTransparent(true);
    floorMaterial->setOpacity(1.0f);
    {
        // Opaque-looking blend (src over) so the floor still reads solid.
        auto blend = std::make_shared<BlendState>();
        blend->setEnabled(true);
        blend->setColorOp(BLENDEQUATION_ADD);
        blend->setColorSrcFactor(BLENDMODE_ONE);
        blend->setColorDstFactor(BLENDMODE_ZERO);
        blend->setAlphaOp(BLENDEQUATION_ADD);
        blend->setAlphaSrcFactor(BLENDMODE_ONE);
        blend->setAlphaDstFactor(BLENDMODE_ZERO);
        floorMaterial->setBlendState(blend);
    }
    engine->root()->addChild(createEntity(
        engine.get(), floorMaterial.get(), "plane", Vector3(0.0f, 0.0f, 0.0f), Vector3(16.0f, 1.0f, 16.0f)));

    spdlog::info("SSR: glossy floor reflecting opaque objects. Auto-toggles SSR ON/OFF every 3 s. Esc quits.");

    bool running = true;
    bool ssrOn = true;
    float elapsed = 0.0f;
    float toggleTimer = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;
        elapsed += static_cast<float>(dtSeconds);

        toggleTimer += static_cast<float>(dtSeconds);
        if (toggleTimer >= 3.0f) {
            toggleTimer = 0.0f;
            ssrOn = !ssrOn;
            floorMaterial->setUseScreenSpaceReflection(ssrOn);
            spdlog::info("SSR: {}", ssrOn ? "ON (screen-space reflections)" : "OFF (env atlas only)");
        }

        // Slow camera sway so the reflections read as 3D.
        camera->setLocalPosition(std::sin(elapsed * 0.25f) * 2.0f, 2.4f, 7.5f);
        camera->setLocalEulerAngles(-16.0f, std::sin(elapsed * 0.25f) * 8.0f, 0.0f);

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
