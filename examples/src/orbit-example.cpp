// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.07.2025.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <iostream>
#include <algorithm>
#include <core/shape/boundingBox.h>
#include <framework/assets/asset.h>

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/constants.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/animation/animationComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"

constexpr int WINDOW_WIDTH = 900;
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

const auto statue = std::make_unique<Asset>(
    "statue",
    AssetType::CONTAINER,
    rootPath + "/models/antique_camera.glb"
);

constexpr int LAYER_IMMEDIATE_EXAMPLE = 5;

BoundingBox calcEntityAABB(Entity* entity)
{
    BoundingBox bbox;
    bbox.setCenter(0, 0, 0);
    bbox.setHalfExtents(0, 0, 0);

    if (!entity) {
        return bbox;
    }

    bool hasAny = false;
    for (auto* render : RenderComponent::instances()) {
        if (!render || !render->entity()) {
            continue;
        }

        auto* owner = render->entity();
        if (owner != entity && !owner->isDescendantOf(entity)) {
            continue;
        }

        for (auto* mi : render->meshInstances()) {
            if (!mi) {
                continue;
            }
            bbox.add(mi->aabb());
            hasAny = true;
        }
    }

    if (!hasAny) {
        bbox.setCenter(entity->position());
        bbox.setHalfExtents(0.5f, 0.5f, 0.5f);
    }
    return bbox;
};

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

    spdlog::info("*** VisuTwin Visualization Engine Started *** ");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Visualization Engine", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
        );
    if (!window)
    {
        std::cerr << "SDL Window Creation Failed: " << std::endl;
        shutdown();
        return -1;
    }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
    {
        std::cerr << "SDL Renderer Creation Failed: " << std::endl;
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain)
    {
        std::cerr << "Unable to get render Metal layer" << std::endl;
        shutdown();
        return -1;
    }

    auto device = createGraphicsDevice(
        GraphicsDeviceOptions{.swapChain = swapchain, .window = window}
    );
    if (!device) {
        std::cerr << "Unable to create graphics device" << std::endl;
        shutdown();
        return -1;
    }

    std::string basePath = ASSET_DIR;
    spdlog::info("Base path: {}", basePath);

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<AnimationComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);

    // Set the canvas to fill the window and automatically change resolution to be the same as the canvas size
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);

    engine->start();

    auto scene = engine->scene();
    scene->setAmbientLight(0.4f, 0.4f, 0.4f);
    scene->setDebugNormalMapsEnabled(false);
    scene->setExposure(1.0f);

    
    scene->setSkyboxMip(1);
    scene->setSkyboxIntensity(0.4f);
    const auto helipadResource = helipad->resource();
    if (!helipadResource)
    {
        spdlog::error("Failed to load helipad texture");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    auto light = new Entity();
    light->setEngine(engine.get());
    light->addComponent<LightComponent>();
    light->setLocalEulerAngles(45, 30, 0);

    engine->root()->addChild(light);
    const auto statueResource = statue->resource();
    if (!statueResource)
    {
        spdlog::error("Failed to load statue model");
        shutdown();
        return -1;
    }
    auto statueEntity = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
    statueEntity->setLocalPosition(0, -0.5, 0);
    engine->root()->addChild(statueEntity);

    const auto bbox = calcEntityAABB(statueEntity);
    const float sceneRadius = std::max(bbox.halfExtents().length(), 1.0f);
    const auto start = Vector3(0.0f, 20.0f, 30.0f);

    auto camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComponentA = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();
    camera->setPosition(start);

    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    const auto sceneSize = sceneRadius;

    cameraControls->setFocusPoint(bbox.center());
    cameraControls->setEnableFly(false);
    cameraControls->setMoveSpeed(2 * sceneSize);
    cameraControls->setMoveFastSpeed(4 * sceneSize);
    cameraControls->setMoveSlowSpeed(sceneSize);
    cameraControls->setOrbitDistance(std::max(sceneRadius * 4.0f, 12.0f));
    cameraControls->storeResetState();

    if (cameraComponentA) {
        auto taa = cameraComponentA->taa();
        taa.enabled = false;
        taa.highQuality = true;
        taa.jitter = 0.7f;
        cameraComponentA->setTaa(taa);
    }

    auto cameraB = new Entity();
    cameraB->setEngine(engine.get());
    auto* cameraComponentB = static_cast<CameraComponent*>(cameraB->addComponent<CameraComponent>());
    cameraB->setPosition(start);
    if (cameraComponentB && cameraComponentB->camera()) {
        cameraComponentB->setLayers({LAYER_IMMEDIATE_EXAMPLE});
        cameraComponentB->camera()->setClearColorBuffer(false);
        cameraComponentB->camera()->setClearDepthBuffer(false);
        cameraComponentB->camera()->setClearStencilBuffer(false);
    }
    engine->root()->addChild(cameraB);

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    auto logTaaState = [&](const char* reason) {
        if (!cameraComponentA) {
            return;
        }
        const auto& taa = cameraComponentA->taa();
        spdlog::info("TAA {}: enabled={}, highQuality={}, jitter={:.2f}",
            reason,
            taa.enabled ? "ON" : "OFF",
            taa.highQuality ? "ON" : "OFF",
            taa.jitter);
    };
    spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
    spdlog::info("Render controls: N normal-map debug, T toggle TAA, Y toggle TAA quality, [ and ] adjust TAA jitter");
    logTaaState("init");

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_N) {
                const bool enabled = !scene->debugNormalMapsEnabled();
                scene->setDebugNormalMapsEnabled(enabled);
                spdlog::info("Normal map debug toggle: {}", enabled ? "ON" : "OFF");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_T && cameraComponentA) {
                auto taa = cameraComponentA->taa();
                taa.enabled = !taa.enabled;
                cameraComponentA->setTaa(taa);
                logTaaState("toggle");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_Y && cameraComponentA) {
                auto taa = cameraComponentA->taa();
                taa.highQuality = !taa.highQuality;
                cameraComponentA->setTaa(taa);
                logTaaState("quality");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_LEFTBRACKET && cameraComponentA) {
                auto taa = cameraComponentA->taa();
                taa.jitter = std::max(0.0f, taa.jitter - 0.1f);
                cameraComponentA->setTaa(taa);
                logTaaState("jitter");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_RIGHTBRACKET && cameraComponentA) {
                auto taa = cameraComponentA->taa();
                taa.jitter = std::min(2.0f, taa.jitter + 0.1f);
                cameraComponentA->setTaa(taa);
                logTaaState("jitter");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(bbox.center(), std::max(sceneRadius * 2.0f, 6.0f));
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

    spdlog::info("*** VisuTwin Visualization Engine Finished *** ");

    return 0;
}
