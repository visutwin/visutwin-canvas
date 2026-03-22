// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// GLB viewer example — loads "A Windy Day" atmospheric visualization model
// as a static scene with orbit camera controls, IBL, and directional lighting.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/animation/animationComponent.h"
#include "framework/components/animation/animationComponentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/entity.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"

constexpr int WINDOW_WIDTH = 1200;
constexpr int WINDOW_HEIGHT = 800;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

const auto glbAsset = std::make_unique<Asset>(
    "fox",
    AssetType::CONTAINER,
    rootPath + "/models/fox.glb"
);

const auto envAtlas = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

BoundingBox calcEntityAABB(Entity* entity)
{
    BoundingBox bbox;
    bbox.setCenter(0.0f, 0.0f, 0.0f);
    bbox.setHalfExtents(0.0f, 0.0f, 0.0f);

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

    spdlog::info("*** VisuTwin Animation Example — A Windy Day ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin — A Windy Day",
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        spdlog::error("SDL Window Creation Failed");
        shutdown();
        return -1;
    }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        spdlog::error("SDL Renderer Creation Failed");
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        spdlog::error("Unable to get render Metal layer");
        shutdown();
        return -1;
    }

    auto device = createGraphicsDevice(
        GraphicsDeviceOptions{.swapChain = swapchain, .window = window}
    );
    if (!device) {
        spdlog::error("Unable to create graphics device");
        shutdown();
        return -1;
    }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<AnimationComponentSystem>();
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
    scene->setSkyboxMip(2);
    scene->setSkyboxIntensity(0.4f);
    scene->setExposure(2.0f);
    scene->setToneMapping(TONEMAP_NEUTRAL);

    // Load environment atlas for IBL
    const auto envAtlasResource = envAtlas->resource();
    if (envAtlasResource) {
        scene->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
    } else {
        spdlog::warn("Failed to load environment atlas — continuing without IBL");
    }

    // -----------------------------------------------------------------------
    // Load the GLB model
    // -----------------------------------------------------------------------
    spdlog::info("Loading 'A Windy Day' GLB model...");
    const auto resource = glbAsset->resource();
    if (!resource) {
        spdlog::error("GLB load failed: asset resource is null");
        shutdown();
        return -1;
    }
    if (!std::holds_alternative<ContainerResource*>(*resource)) {
        spdlog::error("GLB load failed: expected ContainerResource");
        shutdown();
        return -1;
    }

    auto* container = std::get<ContainerResource*>(*resource);
    if (!container) {
        spdlog::error("GLB load failed: container payload is null");
        shutdown();
        return -1;
    }

    auto* modelEntity = container->instantiateRenderEntity();
    if (!modelEntity) {
        spdlog::error("GLB instantiate failed");
        shutdown();
        return -1;
    }
    modelEntity->setEngine(engine.get());
    engine->root()->addChild(modelEntity);

    // Log model stats
    {
        int renderComps = 0, meshInsts = 0;
        for (auto* render : RenderComponent::instances()) {
            if (!render || !render->entity()) continue;
            auto* owner = render->entity();
            if (owner != modelEntity && !owner->isDescendantOf(modelEntity)) continue;
            renderComps++;
            meshInsts += static_cast<int>(render->meshInstances().size());
        }
        spdlog::info("Model loaded: {} render components, {} mesh instances",
            renderComps, meshInsts);
    }

    // Log animation info and configure playback
    {
        auto* animComp = modelEntity->findComponent<AnimationComponent>();
        if (animComp) {
            spdlog::info("Model has {} animations:", animComp->animations().size());
            for (const auto& [name, _] : animComp->animations()) {
                (void)_;
                spdlog::info("  Animation: '{}'", name);
            }

            // "A Windy Day" uses staggered scale spawning — groups appear/disappear
            // in sequence along wind trajectories to simulate particle flow.
            // The animation is designed as a seamless loop.
            // Note: full visual fidelity requires additive blending + color fading
            // (not yet supported); the raw scale animation will show pop-in/out.
        } else {
            spdlog::info("Model has no animations");
        }
    }

    // Auto-frame the model
    const BoundingBox modelBounds = calcEntityAABB(modelEntity);
    const Vector3 center = modelBounds.center();
    const Vector3 halfExt = modelBounds.halfExtents();
    const float radius = std::max(halfExt.length(), 0.5f);
    spdlog::info("Model bounds: center=({:.2f}, {:.2f}, {:.2f}), half=({:.2f}, {:.2f}, {:.2f}), radius={:.2f}",
        center.getX(), center.getY(), center.getZ(),
        halfExt.getX(), halfExt.getY(), halfExt.getZ(), radius);

    // -----------------------------------------------------------------------
    // Lights
    // -----------------------------------------------------------------------
    auto* keyLight = new Entity();
    keyLight->setEngine(engine.get());
    auto* keyLightComp = static_cast<LightComponent*>(keyLight->addComponent<LightComponent>());
    if (keyLightComp) {
        keyLightComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        keyLightComp->setColor(Color(1.0f, 0.97f, 0.92f));
        keyLightComp->setIntensity(1.5f);
        keyLightComp->setCastShadows(true);
        keyLightComp->setShadowResolution(2048);
        keyLightComp->setShadowDistance(std::max(radius * 4.0f, 100.0f));
        keyLightComp->setShadowBias(0.3f);
        keyLightComp->setShadowNormalBias(0.05f);
    }
    keyLight->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
    engine->root()->addChild(keyLight);

    auto* fillLight = new Entity();
    fillLight->setEngine(engine.get());
    auto* fillLightComp = static_cast<LightComponent*>(fillLight->addComponent<LightComponent>());
    if (fillLightComp) {
        fillLightComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        fillLightComp->setColor(Color(0.65f, 0.75f, 1.0f));
        fillLightComp->setIntensity(0.5f);
    }
    fillLight->setLocalEulerAngles(-20.0f, -150.0f, 0.0f);
    engine->root()->addChild(fillLight);

    // -----------------------------------------------------------------------
    // Camera with orbit controls
    // -----------------------------------------------------------------------
    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    cameraEntity->addComponent<ScriptComponent>();

    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setClearColor(Color(0.05f, 0.05f, 0.08f, 1.0f));
        cameraComp->camera()->setFov(55.0f);
        cameraComp->camera()->setNearClip(std::max(0.01f, radius * 0.005f));
        cameraComp->camera()->setFarClip(std::max(500.0f, radius * 20.0f));
    }

    const float camDistance = std::max(radius * 2.8f, 5.0f);
    cameraEntity->setLocalPosition(center + Vector3(0.0f, radius * 0.5f, camDistance));
    engine->root()->addChild(cameraEntity);

    auto* cameraControls = cameraEntity->script()->create<CameraControls>();
    cameraControls->setFocusPoint(center);
    cameraControls->setEnableFly(false);
    cameraControls->setMoveSpeed(radius);
    cameraControls->setMoveFastSpeed(radius * 2.0f);
    cameraControls->setMoveSlowSpeed(radius * 0.5f);
    cameraControls->setOrbitDistance(camDistance);
    cameraControls->storeResetState();

    spdlog::info("Controls: LMB/RMB orbit, Shift/MMB pan, Wheel zoom, F focus, R reset, Esc quit");

    // -----------------------------------------------------------------------
    // Main loop — static model, no per-frame animation
    // -----------------------------------------------------------------------
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
                cameraControls->focus(center, camDistance);
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
        const float dt = static_cast<float>(dtSeconds);

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** VisuTwin Animation Example Finished ***");

    return 0;
}
