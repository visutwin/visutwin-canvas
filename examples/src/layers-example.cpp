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
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;
constexpr int LAYERID_DIAGNOSTICS = 64;

using namespace visutwin::canvas;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;

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

Entity* createPrimitiveEntity(
    Engine* engine, const std::string& type, const Vector3& position, const Vector3& scale, StandardMaterial* material,
    const std::vector<int>& layers)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position);
    entity->setLocalScale(scale);

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setMaterial(material);
        render->setType(type);
        render->setLayers(layers);
    }

    engine->root()->addChild(entity);
    return entity;
}

std::string actionSequenceToString(
    const std::vector<RenderAction*>& actions, CameraComponent* worldCamera, CameraComponent* diagnosticsCamera)
{
    std::ostringstream oss;
    bool first = true;
    for (const auto* action : actions) {
        if (!action || !action->camera || !action->layer) {
            continue;
        }
        if (!first) {
            oss << " -> ";
        }
        first = false;
        const char* cameraName = action->camera == worldCamera
            ? "WorldCamera"
            : (action->camera == diagnosticsCamera ? "DiagnosticsCamera" : "OtherCamera");
        oss << cameraName << ":" << action->layer->id();
    }
    return oss.str();
}

bool validateDrawOrder(
    const std::shared_ptr<LayerComposition>& composition, CameraComponent* worldCamera, CameraComponent* diagnosticsCamera)
{
    if (!composition || !worldCamera || !diagnosticsCamera) {
        return false;
    }

    const auto& actions = composition->renderActions();
    struct ActionKey
    {
        CameraComponent* camera;
        int layerId;
        bool transparent;
    };

    std::vector<ActionKey> actual;
    actual.reserve(actions.size());
    for (const auto* action : actions) {
        if (!action || !action->camera || !action->layer) {
            continue;
        }
        actual.push_back({action->camera, action->layer->id(), action->transparent});
    }

    const std::vector<ActionKey> expected = {
        {worldCamera, LAYERID_WORLD, false},
        {worldCamera, LAYERID_SKYBOX, false},
        {diagnosticsCamera, LAYERID_DIAGNOSTICS, true}
    };

    if (actual.size() != expected.size()) {
        spdlog::error(
            "Layer order validation failed. Expected {} passes, got {}. Actual: {}",
            expected.size(),
            actual.size(),
            actionSequenceToString(actions, worldCamera, diagnosticsCamera)
        );
        return false;
    }

    for (size_t i = 0; i < expected.size(); ++i) {
        if (actual[i].camera != expected[i].camera ||
            actual[i].layerId != expected[i].layerId ||
            actual[i].transparent != expected[i].transparent) {
            spdlog::error(
                "Layer order validation failed at index {}. Actual: {}",
                i,
                actionSequenceToString(actions, worldCamera, diagnosticsCamera)
            );
            return false;
        }
    }

    return true;
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
        "VisuTwin Layered Diagnostics Composition",
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

    auto scene = engine->scene();
    scene->setAmbientLight(0.2f, 0.2f, 0.2f);
    scene->setSkyboxMip(0);

    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad texture");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    const auto defaultComposition = scene->layers();
    if (!defaultComposition) {
        spdlog::error("Default layer composition missing");
        shutdown();
        return -1;
    }

    const auto worldLayer = defaultComposition->getLayerById(LAYERID_WORLD);
    const auto skyboxLayer = defaultComposition->getLayerById(LAYERID_SKYBOX);
    if (!worldLayer || !skyboxLayer) {
        spdlog::error("Required layers (world/skybox) are missing");
        shutdown();
        return -1;
    }

    auto diagnosticsLayer = std::make_shared<Layer>("Diagnostics", LAYERID_DIAGNOSTICS);
    diagnosticsLayer->setClearColorBuffer(false);
    diagnosticsLayer->setClearDepthBuffer(false);
    diagnosticsLayer->setClearStencilBuffer(false);

    auto diagnosticsComposition = std::make_shared<LayerComposition>("diagnostics-composition");
    diagnosticsComposition->pushOpaque(worldLayer);
    diagnosticsComposition->pushOpaque(skyboxLayer);
    diagnosticsComposition->pushTransparent(diagnosticsLayer);
    scene->setLayers(diagnosticsComposition);

    auto worldMaterial = std::make_shared<StandardMaterial>();
    worldMaterial->setDiffuse(Color(1.0f, 0.2f, 0.2f, 1.0f));

    auto diagnosticsMaterial = std::make_shared<StandardMaterial>();
    diagnosticsMaterial->setDiffuse(Color(0.05f, 0.25f, 0.95f, 0.95f));
    diagnosticsMaterial->setEmissive(Color(0.03f, 0.08f, 0.2f, 1.0f));
    diagnosticsMaterial->setEmissiveIntensity(1.0f);
    diagnosticsMaterial->setUseLighting(true);
    diagnosticsMaterial->setOpacity(0.95f);
    diagnosticsMaterial->setTransparent(true);
    auto diagnosticsDepthState = std::make_shared<DepthState>();
    diagnosticsDepthState->setDepthTest(false);
    diagnosticsDepthState->setDepthWrite(false);
    diagnosticsMaterial->setDepthState(diagnosticsDepthState);
    diagnosticsMaterial->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));

    auto* worldBox = createPrimitiveEntity(
        engine.get(), "box", Vector3(0.0f, 0.0f, 0.0f), Vector3(5.0f, 5.0f, 5.0f), worldMaterial.get(), {LAYERID_WORLD}
    );
    auto* diagnosticsBox = createPrimitiveEntity(
        engine.get(),
        "box",
        Vector3(6.0f, 0.0f, 0.0f),
        Vector3(2.5f, 2.5f, 2.5f),
        diagnosticsMaterial.get(),
        {LAYERID_DIAGNOSTICS}
    );

    auto* worldCameraEntity = new Entity();
    worldCameraEntity->setEngine(engine.get());
    auto* worldCamera = static_cast<CameraComponent*>(worldCameraEntity->addComponent<CameraComponent>());
    if (!worldCamera || !worldCamera->camera()) {
        spdlog::error("Failed to create world camera");
        shutdown();
        return -1;
    }
    worldCamera->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
    worldCamera->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
    worldCameraEntity->setLocalPosition(0.0f, 0.0f, 24.0f);
    engine->root()->addChild(worldCameraEntity);

    auto* diagnosticsCameraEntity = new Entity();
    diagnosticsCameraEntity->setEngine(engine.get());
    auto* diagnosticsCamera = static_cast<CameraComponent*>(diagnosticsCameraEntity->addComponent<CameraComponent>());
    if (!diagnosticsCamera || !diagnosticsCamera->camera()) {
        spdlog::error("Failed to create diagnostics camera");
        shutdown();
        return -1;
    }
    diagnosticsCamera->setLayers({LAYERID_DIAGNOSTICS});
    diagnosticsCamera->camera()->setClearColorBuffer(false);
    diagnosticsCamera->camera()->setClearDepthBuffer(false);
    diagnosticsCamera->camera()->setClearStencilBuffer(false);
    diagnosticsCameraEntity->setLocalPosition(0.0f, 0.0f, 24.0f);
    engine->root()->addChild(diagnosticsCameraEntity);

    auto* light = new Entity();
    light->setEngine(engine.get());
    auto* lightComp = static_cast<LightComponent*>(light->addComponent<LightComponent>());
    if (lightComp) {
        lightComp->setType(LightType::LIGHTTYPE_OMNI);
        lightComp->setRange(100.0f);
        lightComp->setIntensity(5.0f);
        lightComp->setLayers({LAYERID_WORLD, LAYERID_DIAGNOSTICS});
    }
    light->setLocalPosition(5.0f, 0.0f, 15.0f);
    engine->root()->addChild(light);

    if (!validateDrawOrder(diagnosticsComposition, worldCamera, diagnosticsCamera)) {
        spdlog::error("Initial deterministic draw-order validation failed");
    } else {
        spdlog::info("Initial deterministic draw-order validation passed");
    }

    bool running = true;
    bool drawOrderValid = true;
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

        worldBox->setLocalEulerAngles(0.0f, time * 10.0f, 0.0f);
        diagnosticsBox->setLocalEulerAngles(0.0f, -time * 10.0f, 0.0f);

        if (drawOrderValid) {
            drawOrderValid = validateDrawOrder(diagnosticsComposition, worldCamera, diagnosticsCamera);
            if (!drawOrderValid) {
                spdlog::error("Draw-order became non-deterministic at runtime");
            }
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();
    return 0;
}
