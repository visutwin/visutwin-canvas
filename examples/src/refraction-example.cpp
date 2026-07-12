// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Dynamic grab-pass refraction demo: a glass sphere refracts the mid-frame scene
// color grab, distorting the colorful column backdrop behind it. Auto-cycles between
// dynamic (grab-pass) refraction and the env-atlas fallback every few seconds
// (1 = dynamic, 2 = env atlas, Space = auto-cycle).
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

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
    const Vector3& position, const Vector3& scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
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
        "VisuTwin Dynamic Refraction", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    scene->setAmbientLight(0.25f, 0.25f, 0.28f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->setLocalPosition(0.0f, 1.2f, 6.0f);
    camera->setLocalEulerAngles(-4.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    // The dynamic-refraction grab pass only runs when the camera publishes the
    // scene color map.
    if (cameraComponent) {
        cameraComponent->requestSceneColorMap(true);
    }

    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lightComponent->setIntensity(1.4f);
    }
    light->setLocalEulerAngles(50.0f, -30.0f, 0.0f);
    engine->root()->addChild(light);

    // Colorful column backdrop — sharp vertical color edges make the refraction
    // distortion unmistakable.
    const Color columnColors[6] = {
        Color(0.9f, 0.15f, 0.15f, 1.0f), Color(0.95f, 0.6f, 0.1f, 1.0f),
        Color(0.9f, 0.85f, 0.1f, 1.0f), Color(0.15f, 0.7f, 0.25f, 1.0f),
        Color(0.15f, 0.4f, 0.9f, 1.0f), Color(0.6f, 0.2f, 0.8f, 1.0f)
    };
    std::vector<std::shared_ptr<StandardMaterial>> columnMaterials;
    for (int i = 0; i < 6; ++i) {
        auto material = std::make_shared<StandardMaterial>();
        material->setName("column-" + std::to_string(i));
        material->setDiffuse(columnColors[i]);
        material->setMetalness(0.0f);
        material->setGlossInvert(true);
        material->setGloss(0.7f);  // roughness
        columnMaterials.push_back(material);
        engine->root()->addChild(createEntity(
            engine.get(), material.get(), "box",
            Vector3(-3.75f + 1.5f * static_cast<float>(i), 1.0f, -2.5f), Vector3(1.2f, 4.0f, 0.6f)
        ));
    }

    // Neutral floor.
    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setName("floor");
    floorMaterial->setDiffuse(Color(0.45f, 0.45f, 0.48f, 1.0f));
    floorMaterial->setMetalness(0.0f);
    floorMaterial->setGlossInvert(true);
    floorMaterial->setGloss(0.9f);
    engine->root()->addChild(createEntity(
        engine.get(), floorMaterial.get(), "plane", Vector3(0.0f, -1.0f, 0.0f), Vector3(24.0f, 1.0f, 24.0f)
    ));

    // The glass sphere: transparent so it renders after the depth-layer grab.
    auto glassMaterial = std::make_shared<StandardMaterial>();
    glassMaterial->setName("glass");
    glassMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
    glassMaterial->setMetalness(0.0f);
    glassMaterial->setGlossInvert(true);
    glassMaterial->setGloss(0.05f);  // near-mirror smooth
    glassMaterial->setTransmissionFactor(1.0f);
    glassMaterial->setRefractionIndex(1.5f);
    glassMaterial->setThickness(0.8f);
    glassMaterial->setUseDynamicRefraction(true);
    glassMaterial->setTransparent(true);
    auto* glass = createEntity(
        engine.get(), glassMaterial.get(), "sphere", Vector3(0.0f, 1.0f, 0.8f), Vector3(2.4f, 2.4f, 2.4f)
    );
    engine->root()->addChild(glass);

    spdlog::info("Dynamic refraction: glass sphere over colorful columns");
    spdlog::info("Keys: 1 = dynamic (grab pass), 2 = env atlas, Space = auto-cycle, Esc = quit");

    bool running = true;
    bool useDynamic = true;
    bool autoCycle = true;
    float cycleTimer = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    const auto applyMode = [&](const bool dynamic) {
        useDynamic = dynamic;
        glassMaterial->setUseDynamicRefraction(dynamic);
        spdlog::info("Refraction mode: {}", dynamic ? "dynamic grab pass" : "env atlas");
    };
    applyMode(true);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_1) {
                autoCycle = false;
                applyMode(true);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_2) {
                autoCycle = false;
                applyMode(false);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                autoCycle = true;
                cycleTimer = 0.0f;
                spdlog::info("Auto-cycle resumed");
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (autoCycle) {
            cycleTimer += static_cast<float>(dtSeconds);
            if (cycleTimer >= 3.0f) {
                cycleTimer = 0.0f;
                applyMode(!useDynamic);
            }
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
