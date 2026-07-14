// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Lightmapper demo: a floor plane baked (on the CPU) with direct sun light,
// hard shadows, and ambient occlusion cast by a few occluder boxes + a sphere.
// The baked lightmap is applied at UV1 (VT_FEATURE_LIGHTMAP); the floor is
// masked out of realtime lighting, so its whole look comes from the bake. The
// occluders are lit by a realtime directional light. Auto-toggles the floor's
// lightmap ON/OFF every 3 s to show the baked shadows appear/disappear. Esc quits.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>
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
#include "framework/lightmapper/lightmapper.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

struct Object { Entity* entity; RenderComponent* render; };

Object createEntity(Engine* engine, Material* material, const char* type,
    const Vector3& position, const Vector3& scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    render->setMaterial(material);
    render->setType(type);
    engine->root()->addChild(entity);
    return {entity, render};
}

Mesh* firstMesh(const Object& obj)
{
    const auto& mis = obj.render->meshInstances();
    return mis.empty() ? nullptr : mis.front()->mesh();
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
        "VisuTwin Lightmap Bake", WINDOW_WIDTH, WINDOW_HEIGHT,
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
    scene->setAmbientLight(0.12f, 0.13f, 0.16f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->setLocalPosition(0.0f, 9.0f, 12.0f);
    camera->setLocalEulerAngles(-35.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    // Floor (lightmapped — masked out of realtime lights).
    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setDiffuse(Color(0.82f, 0.82f, 0.85f, 1.0f));
    floorMaterial->setGlossInvert(true);
    floorMaterial->setGloss(0.9f);
    Object floor = createEntity(engine.get(), floorMaterial.get(), "plane",
        Vector3(0.0f, 0.0f, 0.0f), Vector3(20.0f, 1.0f, 20.0f));

    // Occluders (lit by the realtime light; cast into the bake).
    auto occluderMaterial = std::make_shared<StandardMaterial>();
    occluderMaterial->setDiffuse(Color(0.55f, 0.5f, 0.45f, 1.0f));
    occluderMaterial->setGlossInvert(true);
    occluderMaterial->setGloss(0.7f);
    std::vector<Object> occluders = {
        createEntity(engine.get(), occluderMaterial.get(), "box", Vector3(-2.5f, 1.0f, -1.0f), Vector3(2.0f, 2.0f, 2.0f)),
        createEntity(engine.get(), occluderMaterial.get(), "box", Vector3(2.8f, 0.6f, 1.5f), Vector3(1.2f, 1.2f, 1.2f)),
        createEntity(engine.get(), occluderMaterial.get(), "sphere", Vector3(0.5f, 1.8f, 2.5f), Vector3(2.2f, 2.2f, 2.2f)),
    };

    // ── Bake the floor lightmap ──────────────────────────────────────────────
    Lightmapper lightmapper(graphicsDevice.get());
    Lightmapper::Light sun;
    sun.type = LightType::LIGHTTYPE_DIRECTIONAL;
    sun.direction = Vector3(-0.5f, -1.0f, -0.35f).normalized();  // angled sun
    sun.color = Color(1.0f, 0.96f, 0.88f, 1.0f);
    sun.intensity = 1.6f;
    sun.castShadows = true;
    lightmapper.addLight(sun);

    for (const auto& occ : occluders) {
        if (auto* mesh = firstMesh(occ)) {
            lightmapper.addOccluder(*mesh, occ.entity->worldTransform());
        }
    }

    Lightmapper::Options bakeOptions;
    bakeOptions.lightmapSize = 512;
    bakeOptions.ambient = Color(0.1f, 0.11f, 0.14f, 1.0f);
    bakeOptions.skyColor = Color(0.16f, 0.2f, 0.28f, 1.0f);  // AO-modulated sky term
    bakeOptions.ambientOcclusion = true;
    bakeOptions.aoSamples = 24;
    bakeOptions.aoRadius = 6.0f;
    bakeOptions.dilatePixels = 6;

    std::shared_ptr<Texture> floorLightmap;
    if (auto* floorMesh = firstMesh(floor)) {
        spdlog::info("Baking floor lightmap...");
        floorLightmap = lightmapper.bake(*floorMesh, floor.entity->worldTransform(), bakeOptions);
        floorMaterial->setLightMap(floorLightmap.get());
    }

    // Mask the floor out of realtime lighting — its look comes from the bake.
    for (auto* mi : floor.render->meshInstances()) {
        mi->setMask(MASK_AFFECT_LIGHTMAPPED);
    }

    // Realtime sun for the occluders (matches the baked sun direction).
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 0.96f, 0.88f, 1.0f));
        lightComponent->setIntensity(1.6f);
        lightComponent->setMask(MASK_AFFECT_DYNAMIC);
    }
    light->setLocalEulerAngles(58.0f, -35.0f, 0.0f);  // ~ matches sun.direction
    engine->root()->addChild(light);

    spdlog::info("Lightmap bake demo. Floor lightmap auto-toggles ON/OFF every 3 s. Esc quits.");

    bool running = true;
    bool lightmapOn = true;
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

        toggleTimer += static_cast<float>(dtSeconds);
        if (toggleTimer >= 3.0f) {
            toggleTimer = 0.0f;
            lightmapOn = !lightmapOn;
            floorMaterial->setLightMap(lightmapOn ? floorLightmap.get() : nullptr);
            spdlog::info("Floor lightmap: {}", lightmapOn ? "ON (baked shadows + AO)" : "OFF");
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
