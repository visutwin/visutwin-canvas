// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <iostream>
#include <algorithm>
#include <cmath>
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
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Assets matching upstream TAA example
const auto envAtlas = std::make_unique<Asset>(
    "table-mountain-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/table-mountain-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

const auto house = std::make_unique<Asset>(
    "house",
    AssetType::CONTAINER,
    rootPath + "/models/da_vinci_workshop.glb"
);

const auto cube = std::make_unique<Asset>(
    "cube",
    AssetType::CONTAINER,
    rootPath + "/models/box_textured.glb"
);

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

    spdlog::info("*** VisuTwin TAA Example Started ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin TAA Example", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        std::cerr << "SDL Window Creation Failed" << std::endl;
        shutdown();
        return -1;
    }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::cerr << "SDL Renderer Creation Failed" << std::endl;
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
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

    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);

    engine->start();

    auto scene = engine->scene();

    // setup skydome
    scene->setSkyboxMip(0);
    scene->setExposure(1.6f);
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.2f, 0.2f, 0.2f);

    // add shadow casting directional light
    auto light = new Entity();
    light->setEngine(engine.get());
    auto* lightComp = static_cast<LightComponent*>(light->addComponent<LightComponent>());
    if (lightComp) {
        lightComp->setCastShadows(true);
        lightComp->setShadowResolution(4096);
        lightComp->setShadowDistance(600.0f);
        lightComp->setShadowBias(0.2f);
        lightComp->setShadowNormalBias(0.05f);
    }
    light->setLocalEulerAngles(40, 10, 0);
    engine->root()->addChild(light);

    // ── Load resources synchronously ──────────────────────────────────────
    const auto envAtlasResource = envAtlas->resource();
    if (envAtlasResource) {
        scene->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
    } else {
        spdlog::warn("Failed to load environment atlas — continuing without IBL");
    }

    const auto houseResource = house->resource();
    if (!houseResource) {
        spdlog::error("Failed to load house model");
        shutdown();
        return -1;
    }
    auto* houseEntity = std::get<ContainerResource*>(*houseResource)->instantiateRenderEntity();
    houseEntity->setLocalScale(100, 100, 100);
    engine->root()->addChild(houseEntity);

    const auto cubeResource = cube->resource();
    Entity* cubeEntity = nullptr;
    if (cubeResource) {
        cubeEntity = std::get<ContainerResource*>(*cubeResource)->instantiateRenderEntity();
        cubeEntity->setLocalScale(30, 30, 30);
        engine->root()->addChild(cubeEntity);
    }

    // Auto-frame the house model
    const auto houseBbox = calcEntityAABB(houseEntity);
    const float sceneRadius = std::max(houseBbox.halfExtents().length(), 1.0f);

    // create camera entity
    auto camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();

    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setNearClip(std::max(1.0f, sceneRadius * 0.01f));
        cameraComp->camera()->setFarClip(std::max(600.0f, sceneRadius * 6.0f));
        cameraComp->camera()->setFov(80.0f);
    }

    // TAA enabled by default, jitter=1.0
    if (cameraComp) {
        auto taa = cameraComp->taa();
        taa.enabled = true;
        taa.highQuality = true;
        taa.jitter = 1.0f;
        cameraComp->setTaa(taa);

        auto rendering = cameraComp->rendering();
        rendering.bloomIntensity = 0.02f;
        rendering.sharpness = 0.5f;
        rendering.renderTargetScale = 1.0f;
        rendering.toneMapping = TONEMAP_ACES;
        cameraComp->setRendering(rendering);
    }

    const float orbitDist = std::max(sceneRadius * 2.0f, 220.0f);
    camera->setPosition(houseBbox.center() + Vector3(0.0f, sceneRadius * 0.3f, orbitDist));
    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(houseBbox.center());
    cameraControls->setEnableFly(false);
    cameraControls->setAutoFarClip(true);
    cameraControls->setMoveSpeed(2 * sceneRadius);
    cameraControls->setMoveFastSpeed(4 * sceneRadius);
    cameraControls->setMoveSlowSpeed(sceneRadius);
    cameraControls->setOrbitDistance(orbitDist);
    cameraControls->storeResetState();

    spdlog::info("House AABB center=({:.1f},{:.1f},{:.1f}), radius={:.1f}, orbit={:.1f}",
        houseBbox.center().getX(), houseBbox.center().getY(), houseBbox.center().getZ(),
        sceneRadius, orbitDist);

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float totalTime = 0.0f;
    float cubeRotX = 0.0f, cubeRotY = 0.0f, cubeRotZ = 0.0f;

    // Helper lambdas for logging state
    auto logTaaState = [&](const char* reason) {
        if (!cameraComp) {
            return;
        }
        const auto& taa = cameraComp->taa();
        spdlog::info("TAA {}: enabled={}, highQuality={}, jitter={:.2f}",
            reason,
            taa.enabled ? "ON" : "OFF",
            taa.highQuality ? "ON" : "OFF",
            taa.jitter);
    };

    auto logRenderingState = [&](const char* reason) {
        if (!cameraComp) {
            return;
        }
        const auto& rendering = cameraComp->rendering();
        spdlog::info("Rendering {}: bloom={:.3f}, sharpness={:.2f}, scale={:.1f}, tonemap={}",
            reason,
            rendering.bloomIntensity,
            rendering.sharpness,
            rendering.renderTargetScale,
            rendering.toneMapping == TONEMAP_ACES ? "ACES" :
            rendering.toneMapping == TONEMAP_NONE ? "NONE" : "LINEAR");
    };

    spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
    spdlog::info("TAA controls: T toggle TAA, Y toggle quality, [ ] adjust jitter");
    spdlog::info("Render controls: B toggle bloom, S cycle sharpness, M cycle tonemap, -/= adjust scale, N normal debug");
    logTaaState("init");
    logRenderingState("init");

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;

            // TAA controls
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_T && cameraComp) {
                auto taa = cameraComp->taa();
                taa.enabled = !taa.enabled;
                cameraComp->setTaa(taa);
                logTaaState("toggle");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_Y && cameraComp) {
                auto taa = cameraComp->taa();
                taa.highQuality = !taa.highQuality;
                cameraComp->setTaa(taa);
                logTaaState("quality");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_LEFTBRACKET && cameraComp) {
                auto taa = cameraComp->taa();
                taa.jitter = std::max(0.0f, taa.jitter - 0.05f);
                cameraComp->setTaa(taa);
                logTaaState("jitter-");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_RIGHTBRACKET && cameraComp) {
                auto taa = cameraComp->taa();
                taa.jitter = std::min(1.0f, taa.jitter + 0.05f);
                cameraComp->setTaa(taa);
                logTaaState("jitter+");

            // Rendering controls
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_B && cameraComp) {
                auto rendering = cameraComp->rendering();
                rendering.bloomIntensity = rendering.bloomIntensity > 0.0f ? 0.0f : 0.02f;
                cameraComp->setRendering(rendering);
                logRenderingState("bloom");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_S && cameraComp) {
                // Cycle sharpness: 0.0 -> 0.25 -> 0.5 -> 0.75 -> 1.0 -> 0.0
                auto rendering = cameraComp->rendering();
                if (rendering.sharpness < 0.125f) rendering.sharpness = 0.25f;
                else if (rendering.sharpness < 0.375f) rendering.sharpness = 0.5f;
                else if (rendering.sharpness < 0.625f) rendering.sharpness = 0.75f;
                else if (rendering.sharpness < 0.875f) rendering.sharpness = 1.0f;
                else rendering.sharpness = 0.0f;
                cameraComp->setRendering(rendering);
                logRenderingState("sharpness");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_M && cameraComp) {
                // Cycle tonemapping: LINEAR -> ACES -> NONE -> LINEAR
                auto rendering = cameraComp->rendering();
                if (rendering.toneMapping == TONEMAP_LINEAR) rendering.toneMapping = TONEMAP_ACES;
                else if (rendering.toneMapping == TONEMAP_ACES) rendering.toneMapping = TONEMAP_NONE;
                else rendering.toneMapping = TONEMAP_LINEAR;
                cameraComp->setRendering(rendering);
                // Also update scene tonemapping
                scene->setToneMapping(rendering.toneMapping);
                logRenderingState("tonemap");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_MINUS && cameraComp) {
                auto rendering = cameraComp->rendering();
                rendering.renderTargetScale = std::max(0.5f, rendering.renderTargetScale - 0.1f);
                cameraComp->setRendering(rendering);
                logRenderingState("scale-");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_EQUALS && cameraComp) {
                auto rendering = cameraComp->rendering();
                rendering.renderTargetScale = std::min(1.0f, rendering.renderTargetScale + 0.1f);
                cameraComp->setRendering(rendering);
                logRenderingState("scale+");

            // Scene controls
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_N) {
                const bool enabled = !scene->debugNormalMapsEnabled();
                scene->setDebugNormalMapsEnabled(enabled);
                spdlog::info("Normal map debug toggle: {}", enabled ? "ON" : "OFF");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(houseBbox.center(), orbitDist);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();

            // Zoom
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
        const float dt = static_cast<float>(dtSeconds);

        // animate the cube — orbit + rotate
        totalTime += dt;
        if (cubeEntity) {
            cubeEntity->setLocalPosition(
                130.0f * std::sin(totalTime),
                0.0f,
                130.0f * std::cos(totalTime)
            );
            cubeRotX += 50.0f * dt;
            cubeRotY += 20.0f * dt;
            cubeRotZ += 30.0f * dt;
            cubeEntity->setLocalEulerAngles(cubeRotX, cubeRotY, cubeRotZ);
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** VisuTwin TAA Example Finished ***");

    return 0;
}
