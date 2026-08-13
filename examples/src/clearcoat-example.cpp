// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Clearcoat material demo (parity with PlayCanvas materials/clear-coat): the
// Khronos ClearCoatTest.glb sample asset — six labelled columns of sphere/plane
// pairs comparing Base / Coating / Coated variants (partial coat masks, rough
// coat variations, base/coat/shared normal maps) — lit by the morning env atlas
// and a yellow directional light. The Coated column shows highlights from BOTH
// the base and coating layers.
//
// The GLB's materials author clearcoat via KHR_materials_clearcoat (factors +
// intensity/roughness/normal textures), parsed by glbParser::applyClearcoat.
// DEVIATION: no Scene::setSkyboxRotation API, so upstream's 70° skydome yaw is
// skipped (background orientation only).
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <algorithm>
#include <memory>

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
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

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Morning environment atlas — same asset as the upstream example.
const auto morning = std::make_unique<Asset>(
    "morning-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/morning-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

// Khronos ClearCoatTest sample model (KHR_materials_clearcoat).
const auto model = std::make_unique<Asset>(
    "clearcoat-test",
    AssetType::CONTAINER,
    rootPath + "/models/ClearCoatTest.glb"
);

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
        "Clear Coat", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    scene->setSkyboxIntensity(1.5f);

    const auto morningResource = morning->resource();
    if (!morningResource) {
        spdlog::error("Failed to load morning env atlas");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*morningResource));

    // ClearCoatTest model, posed like upstream: yaw 90, position (0,0,1), scale 0.8.
    const auto modelResource = model->resource();
    if (!modelResource) {
        spdlog::error("Failed to load ClearCoatTest.glb");
        shutdown();
        return -1;
    }
    auto* modelEntity = std::get<ContainerResource*>(*modelResource)->instantiateRenderEntity();
    modelEntity->setLocalEulerAngles(0.0f, 90.0f, 0.0f);
    modelEntity->setLocalPosition(0.0f, 0.0f, 1.0f);
    modelEntity->setLocalScale(0.8f, 0.8f, 0.8f);
    engine->root()->addChild(modelEntity);

    // Yellow directional light, no shadows (upstream).
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        lightComponent->setColor(Color(1.0f, 1.0f, 0.0f, 1.0f));
        lightComponent->setIntensity(1.0f);
        lightComponent->setCastShadows(false);
    }
    light->setLocalEulerAngles(45.0f, 180.0f, 0.0f);
    engine->root()->addChild(light);

    // Orbit camera: upstream orbitCamera yaw 90, distance 12 around the model.
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->addComponent<ScriptComponent>();
    camera->setPosition(Vector3(12.0f, 0.0f, 1.0f));
    camera->setLocalEulerAngles(0.0f, 90.0f, 0.0f);
    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(Vector3(0.0f, 0.0f, 1.0f));
    cameraControls->setEnableFly(false);
    cameraControls->storeResetState();

    spdlog::info("Clear coat: ClearCoatTest.glb (KHR_materials_clearcoat) — the Coated column "
                 "carries highlights from both Base and Coating layers.");
    spdlog::info("Orbit: LMB/RMB orbit, Wheel zoom, R reset, Esc quit.");

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            } else if (event.type == SDL_EVENT_PINCH_UPDATE && cameraControls) {
                cameraControls->addZoomInput((event.pinch.scale - 1.0f) * 10.0f);
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
