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
#include <iostream>
#include <memory>
#include <vector>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/animation/animationComponentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;
constexpr int MONITOR_LAYER_ID = visutwin::canvas::LAYERID_UI;

using namespace visutwin::canvas;

SDL_Window* window;
SDL_Renderer* renderer;

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

const auto checkerboard = std::make_unique<Asset>(
    "checkerboard",
    AssetType::TEXTURE,
    rootPath + "/textures/checkboard.png"
);

void setLookAt(Entity* camera, const Vector3& target)
{
    if (!camera) {
        return;
    }

    const Vector3 position = camera->position();
    const Vector3 lookDir = (target - position).normalized();
    const float pitchDeg = std::asin(std::clamp(lookDir.getY(), -1.0f, 1.0f)) * RAD_TO_DEG;
    const float yawDeg = std::atan2(-lookDir.getX(), -lookDir.getZ()) * RAD_TO_DEG;
    camera->setLocalEulerAngles(pitchDeg, yawDeg, 0.0f);
}

Entity* createPrimitiveEntity(
    Engine* engine, const std::string& type, const Vector3& position, const Vector3& scale, StandardMaterial* material,
    const std::vector<int>& layers = {LAYERID_WORLD})
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
        "VisuTwin Render-To-Texture Sensor View",
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
    createOptions.registerComponentSystem<AnimationComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setSkyboxMip(0);

    const auto helipadResource = helipad->resource();
    const auto checkerResource = checkerboard->resource();
    if (!helipadResource || !checkerResource) {
        spdlog::error("Failed to load required textures");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    auto checkerMaterial = std::make_shared<StandardMaterial>();
    checkerMaterial->setDiffuse(Color(0.8f, 0.8f, 0.8f, 1.0f));
    checkerMaterial->setDiffuseMap(std::get<Texture*>(*checkerResource));

    auto redMaterial = std::make_shared<StandardMaterial>();
    redMaterial->setDiffuse(Color(1.0f, 0.1f, 0.1f, 1.0f));
    auto cyanMaterial = std::make_shared<StandardMaterial>();
    cyanMaterial->setDiffuse(Color(0.1f, 1.0f, 1.0f, 1.0f));
    auto yellowMaterial = std::make_shared<StandardMaterial>();
    yellowMaterial->setDiffuse(Color(1.0f, 1.0f, 0.1f, 1.0f));

    // World layer scene objects.
    auto* ground = createPrimitiveEntity(
        engine.get(), "plane", Vector3(0.0f, 0.0f, 0.0f), Vector3(20.0f, 20.0f, 20.0f), checkerMaterial.get()
    );
    (void)ground;
    auto* sphere = createPrimitiveEntity(
        engine.get(), "sphere", Vector3(-2.0f, 1.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f), redMaterial.get()
    );
    auto* cone = createPrimitiveEntity(
        engine.get(), "cone", Vector3(0.0f, 1.0f, -2.0f), Vector3(2.0f, 2.0f, 2.0f), cyanMaterial.get()
    );
    auto* box = createPrimitiveEntity(
        engine.get(), "box", Vector3(2.0f, 1.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f), yellowMaterial.get()
    );

    // Offscreen sensor texture and render target.
    TextureOptions sensorTextureOptions;
    sensorTextureOptions.name = "SensorRT";
    sensorTextureOptions.width = 512;
    sensorTextureOptions.height = 256;
    sensorTextureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
    sensorTextureOptions.mipmaps = false;
    sensorTextureOptions.minFilter = FilterMode::FILTER_LINEAR;
    sensorTextureOptions.magFilter = FilterMode::FILTER_LINEAR;
    auto sensorTexture = std::make_shared<Texture>(graphicsDevice.get(), sensorTextureOptions);
    sensorTexture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
    sensorTexture->setAddressV(ADDRESS_CLAMP_TO_EDGE);

    RenderTargetOptions sensorRtOptions;
    sensorRtOptions.graphicsDevice = graphicsDevice.get();
    sensorRtOptions.colorBuffer = sensorTexture.get();
    sensorRtOptions.depth = true;
    sensorRtOptions.name = "SensorRenderTarget";
    auto sensorRenderTarget = graphicsDevice->createRenderTarget(sensorRtOptions);

    // Monitor mesh in UI layer so it is visible to main camera only (texture camera excludes UI).
    auto monitorMaterial = std::make_shared<StandardMaterial>();
    monitorMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
    monitorMaterial->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
    monitorMaterial->setEmissiveMap(sensorTexture.get());
    auto* monitor = createPrimitiveEntity(
        engine.get(),
        "plane",
        Vector3(6.0f, 8.0f, -5.0f),
        Vector3(20.0f, 10.0f, 10.0f),
        monitorMaterial.get(),
        {MONITOR_LAYER_ID}
    );
    monitor->setLocalEulerAngles(90.0f, 0.0f, 0.0f);

    // Main camera renders world + skybox + UI(defaults from CameraComponent).
    auto* mainCameraEntity = new Entity();
    mainCameraEntity->setEngine(engine.get());
    auto* mainCamera = static_cast<CameraComponent*>(mainCameraEntity->addComponent<CameraComponent>());
    if (mainCamera && mainCamera->camera()) {
        mainCamera->camera()->setFov(100.0f);
        mainCamera->camera()->setClearColor(Color(0.2f, 0.2f, 0.25f, 1.0f));
    }
    mainCameraEntity->setLocalPosition(0.0f, 9.0f, 15.0f);
    setLookAt(mainCameraEntity, Vector3(1.0f, 4.0f, 0.0f));
    engine->root()->addChild(mainCameraEntity);

    // Texture camera renders only world + skybox into the sensor target.
    auto* textureCameraEntity = new Entity();
    textureCameraEntity->setEngine(engine.get());
    auto* textureCamera = static_cast<CameraComponent*>(textureCameraEntity->addComponent<CameraComponent>());
    if (textureCamera && textureCamera->camera()) {
        textureCamera->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
        textureCamera->camera()->setRenderTarget(sensorRenderTarget);
        textureCamera->camera()->setClearColor(Color(0.05f, 0.05f, 0.05f, 1.0f));
    }
    textureCameraEntity->setLocalPosition(0.0f, 2.0f, 5.0f);
    setLookAt(textureCameraEntity, Vector3(0.0f, 0.0f, 0.0f));
    engine->root()->addChild(textureCameraEntity);

    // World omni light (shared by both cameras through world layer).
    auto* light = new Entity();
    light->setEngine(engine.get());
    auto* lightComp = static_cast<LightComponent*>(light->addComponent<LightComponent>());
    if (lightComp) {
        lightComp->setType(LightType::LIGHTTYPE_OMNI);
        lightComp->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lightComp->setRange(200.0f);
        lightComp->setIntensity(5.0f);
        lightComp->setLayers({LAYERID_WORLD});
    }
    light->setLocalPosition(0.0f, 2.0f, 5.0f);
    engine->root()->addChild(light);

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

        const uint64_t currentCounter = SDL_GetPerformanceCounter();
        float deltaTime = static_cast<float>(currentCounter - prevCounter) / static_cast<float>(perfFreq);
        prevCounter = currentCounter;
        deltaTime = std::clamp(deltaTime, 0.0f, 0.1f);
        time += deltaTime;

        // Animate sensor camera and scene objects to validate live monitor feed.
        textureCameraEntity->setLocalPosition(8.0f * std::sin(time * 0.9f), 4.0f, 8.0f * std::cos(time * 0.9f));
        setLookAt(textureCameraEntity, Vector3(0.0f, 1.0f, 0.0f));

        sphere->setLocalPosition(-2.0f, 1.0f + 0.35f * std::sin(time * 1.8f), 0.0f);
        cone->setLocalEulerAngles(0.0f, time * 45.0f, 0.0f);
        box->setLocalEulerAngles(time * 30.0f, time * 20.0f, 0.0f);

        light->setLocalPosition(6.0f * std::sin(time * 0.7f), 3.0f, 6.0f * std::cos(time * 0.7f));

        engine->update(deltaTime);
        engine->render();
    }

    shutdown();
    return 0;
}
