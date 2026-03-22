// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
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

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/entity.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/graphicsDeviceCreate.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;
const auto glbAsset = std::make_unique<Asset>(
    "statue",
    AssetType::CONTAINER,
    rootPath + "/models/antique_camera.glb"
);

struct RenderableStats
{
    int renderComponents = 0;
    int meshInstances = 0;
};

void gatherRenderableStats(GraphNode* node, RenderableStats& stats)
{
    if (!node) {
        return;
    }

    if (auto* entity = dynamic_cast<Entity*>(node)) {
        if (auto* render = entity->findComponent<RenderComponent>()) {
            stats.renderComponents++;
            stats.meshInstances += static_cast<int>(render->meshInstances().size());
        }
    }

    for (auto* child : node->children()) {
        gatherRenderableStats(child, stats);
    }
}

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

void setLookAt(Entity* cameraEntity, const Vector3& target)
{
    if (!cameraEntity) {
        return;
    }
    const Vector3 position = cameraEntity->position();
    const Vector3 lookDir = (target - position).normalized();
    const float pitchDeg = std::asin(std::clamp(lookDir.getY(), -1.0f, 1.0f)) * RAD_TO_DEG;
    const float yawDeg = std::atan2(-lookDir.getX(), -lookDir.getZ()) * RAD_TO_DEG;
    cameraEntity->setLocalEulerAngles(pitchDeg, yawDeg, 0.0f);
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

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin GLB Loader Reference",
        WINDOW_WIDTH, WINDOW_HEIGHT,
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
    createOptions.registerComponentSystem<LightComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    // Camera setup
    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    if (!camera || !camera->camera()) {
        spdlog::error("Failed to create camera component");
        shutdown();
        return -1;
    }
    camera->camera()->setClearColor(Color(0.08f, 0.1f, 0.14f, 1.0f));
    camera->camera()->setFov(55.0f);
    engine->root()->addChild(cameraEntity);

    // Light setup
    auto* lightEntity = new Entity();
    lightEntity->setEngine(engine.get());
    auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
    if (light) {
        light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        light->setIntensity(1.6f);
        light->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
    }
    lightEntity->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
    engine->root()->addChild(lightEntity);

    const auto resource = glbAsset->resource();
    if (!resource) {
        spdlog::error("GLB load failed: asset resource is null");
        spdlog::error("Diagnostics: parser logs above include unsupported/invalid payload reasons.");
        shutdown();
        return -1;
    }
    if (!std::holds_alternative<ContainerResource*>(*resource)) {
        spdlog::error("GLB load failed: expected ContainerResource, got different asset payload type");
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
        spdlog::error("GLB instantiate failed: instantiateRenderEntity returned null");
        shutdown();
        return -1;
    }
    modelEntity->setEngine(engine.get());
    engine->root()->addChild(modelEntity);

    RenderableStats stats;
    gatherRenderableStats(modelEntity, stats);
    spdlog::info(
        "GLB instantiate result: renderComponents={}, meshInstances={}",
        stats.renderComponents,
        stats.meshInstances
    );
    if (stats.meshInstances == 0) {
        spdlog::error("GLB diagnostics: model instantiated but produced no mesh instances.");
        spdlog::error("Likely unsupported payload for current parser path (extensions, accessor formats, or non-mesh content).");
        spdlog::error("See parser warnings above for unsupported textures/images/accessors.");
    }

    // Auto-frame the model so very large/small assets remain visible.
    const BoundingBox modelBounds = calcEntityAABB(modelEntity);
    const Vector3 center = modelBounds.center();
    const float radius = std::max(modelBounds.halfExtents().length(), 0.5f);
    const float distance = std::max(radius * 2.8f, 3.0f);
    cameraEntity->setLocalPosition(center + Vector3(0.0f, radius * 0.6f, distance));
    setLookAt(cameraEntity, center);
    camera->camera()->setNearClip(std::max(0.01f, radius * 0.01f));
    camera->camera()->setFarClip(std::max(500.0f, radius * 40.0f));

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float time = 0.0f;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;
        const float dt = static_cast<float>(dtSeconds);
        time += dt;

        modelEntity->setLocalEulerAngles(0.0f, time * 18.0f, 0.0f);

        engine->update(dt);
        engine->render();
    }

    shutdown();
    return 0;
}
