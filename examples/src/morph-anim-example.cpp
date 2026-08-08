// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Morph weight animation + skinned culling demo. Loads morph_wave.glb (a plane whose
// two morph targets are driven by a glTF "weights" animation channel) and fox.glb
// (skinned Run cycle). Self-test logs: the animated morph weights, the fox's
// bone-driven world AABB in two poses, and the forward-pass GPU time with the fox
// in view versus culled (camera turned away).
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
#include <vector>

#include "framework/assets/asset.h"
#include "framework/components/animation/animationComponent.h"
#include "framework/components/animation/animationComponentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "platform/graphics/gpuProfiler.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/morphInstance.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// An Asset owns the textures, materials and meshes it loaded, and the entity that
// instantiateRenderEntity() returns only holds RAW pointers into them (Material
// stores Texture* — see the "remains valid until its Asset is" note in asset.h).
// Keeping the Asset in a local unique_ptr therefore freed every texture the moment
// loadGlb() returned, and the next frame's bindMaterialTextures dereferenced them:
// an intermittent use-after-free that crashed roughly 3 runs in 5 on both backends.
// Every other example keeps its Assets at file scope; these live just as long.
std::vector<std::unique_ptr<Asset>> loadedAssets;

Entity* loadGlb(Engine* engine, const char* name, const std::string& path)
{
    auto& asset = loadedAssets.emplace_back(
        std::make_unique<Asset>(name, AssetType::CONTAINER, path, AssetData{}));
    const auto resource = asset->resource();
    if (!resource) {
        spdlog::error("Failed to load {}", path);
        return nullptr;
    }
    auto* container = std::get<ContainerResource*>(*resource);
    if (!container) {
        return nullptr;
    }
    auto* entity = container->instantiateRenderEntity();
    if (entity) {
        entity->setEngine(engine);
        engine->root()->addChild(entity);
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
        "VisuTwin Morph Weights + Skinned Culling", WINDOW_WIDTH, WINDOW_HEIGHT,
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

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<AnimationComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.3f, 0.3f, 0.33f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->setLocalPosition(0.0f, 60.0f, 220.0f);
    camera->setLocalEulerAngles(-12.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lightComponent->setIntensity(1.3f);
    }
    light->setLocalEulerAngles(45.0f, -30.0f, 0.0f);
    engine->root()->addChild(light);

    // Morphing plane (weights-animated) — scaled up and tilted toward the camera.
    auto* morphEntity = loadGlb(engine.get(), "morph-wave", rootPath + "/models/morph_wave.glb");
    if (morphEntity) {
        morphEntity->setLocalPosition(-70.0f, 40.0f, 0.0f);
        morphEntity->setLocalScale(45.0f, 45.0f, 45.0f);
        morphEntity->setLocalEulerAngles(-55.0f, 0.0f, 0.0f);
        if (auto* animComponent = morphEntity->findComponent<AnimationComponent>()) {
            animComponent->play("WaveCycle");
            spdlog::info("Playing 'WaveCycle' (morph weights animation)");
        } else {
            spdlog::warn("morph_wave.glb: no AnimationComponent instantiated");
        }
    }

    // Fox (skinned Run cycle) for bone-AABB culling verification.
    Entity* foxEntity = loadGlb(engine.get(), "fox", rootPath + "/models/fox.glb");
    if (foxEntity) {
        foxEntity->setLocalPosition(60.0f, 0.0f, 0.0f);
        if (auto* animComponent = foxEntity->findComponent<AnimationComponent>()) {
            animComponent->play("Run");
            spdlog::info("Playing 'Run' (fox skinned animation)");
        }
    }

    // Locate the morph instance + fox skinned mesh instance for the self-test.
    MorphInstance* morphInstance = nullptr;
    MeshInstance* foxMeshInstance = nullptr;
    for (auto* render : RenderComponent::instances()) {
        if (!render || !render->entity()) continue;
        auto* owner = render->entity();
        for (auto* meshInstance : render->meshInstances()) {
            if (!meshInstance) continue;
            if (!morphInstance && meshInstance->morphInstance() &&
                morphEntity && (owner == morphEntity || owner->isDescendantOf(morphEntity))) {
                morphInstance = meshInstance->morphInstance();
            }
            if (!foxMeshInstance && meshInstance->skinInstance() &&
                foxEntity && (owner == foxEntity || owner->isDescendantOf(foxEntity))) {
                foxMeshInstance = meshInstance;
            }
        }
    }
    spdlog::info("Self-test handles: morphInstance={} foxSkinnedMesh={} (cull={})",
        morphInstance != nullptr, foxMeshInstance != nullptr,
        foxMeshInstance ? foxMeshInstance->cull() : false);

    if (const auto& profiler = graphicsDevice->gpuProfiler()) {
        profiler->setEnabled(true);
    }

    bool running = true;
    float elapsed = 0.0f;
    float logTimer = 0.0f;
    bool aabbLoggedA = false, aabbLoggedB = false, turnedAway = false;
    double inViewMs = 0.0, awayMs = 0.0;
    int inViewSamples = 0, awaySamples = 0;
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

        // 1 Hz morph weight log for the first 4 seconds.
        logTimer += static_cast<float>(dtSeconds);
        if (morphInstance && elapsed < 4.5f && logTimer >= 1.0f) {
            logTimer = 0.0f;
            spdlog::info("t={:.1f}s morph weights: [{:.3f}, {:.3f}]",
                elapsed, morphInstance->weight(0), morphInstance->weight(1));
        }

        // Fox bone-driven AABB in two different poses.
        if (foxMeshInstance && !aabbLoggedA && elapsed > 1.0f) {
            aabbLoggedA = true;
            const auto aabb = foxMeshInstance->aabb();
            spdlog::info("fox AABB @1.0s center=({:.1f},{:.1f},{:.1f}) halfExtents=({:.1f},{:.1f},{:.1f})",
                aabb.center().getX(), aabb.center().getY(), aabb.center().getZ(),
                aabb.halfExtents().getX(), aabb.halfExtents().getY(), aabb.halfExtents().getZ());
        }
        if (foxMeshInstance && !aabbLoggedB && elapsed > 1.4f) {
            aabbLoggedB = true;
            const auto aabb = foxMeshInstance->aabb();
            spdlog::info("fox AABB @1.4s center=({:.1f},{:.1f},{:.1f}) halfExtents=({:.1f},{:.1f},{:.1f})",
                aabb.center().getX(), aabb.center().getY(), aabb.center().getZ(),
                aabb.halfExtents().getX(), aabb.halfExtents().getY(), aabb.halfExtents().getZ());
        }

        // GPU cost with everything in view (5-6s) vs camera turned away (7-9s):
        // culled skinned meshes should drop the forward-pass time.
        if (const auto& profiler = graphicsDevice->gpuProfiler(); profiler && profiler->enabled()) {
            for (const auto& pass : profiler->passTimings()) {
                if (pass.name.find("Forward") == std::string::npos) continue;
                if (elapsed > 5.0f && elapsed < 6.0f) { inViewMs += pass.milliseconds; inViewSamples++; }
                if (elapsed > 7.0f && elapsed < 9.0f) { awayMs += pass.milliseconds; awaySamples++; }
            }
        }
        if (!turnedAway && elapsed > 6.5f) {
            turnedAway = true;
            camera->setLocalEulerAngles(-12.0f, 180.0f, 0.0f);
            spdlog::info("Camera turned away (everything behind the camera)");
        }
        if (elapsed > 9.0f) {
            if (inViewSamples > 0 && awaySamples > 0) {
                spdlog::info("Forward pass GPU: in-view {:.3f} ms avg ({} samples), away {:.3f} ms avg ({} samples)",
                    inViewMs / inViewSamples, inViewSamples, awayMs / awaySamples, awaySamples);
            }
            running = false;
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
