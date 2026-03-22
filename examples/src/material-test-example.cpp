// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <iostream>
#include <memory>

#include <QuartzCore/QuartzCore.hpp>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/animation/animationComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "framework/assets/asset.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;
constexpr float ORBIT_RADIUS = 4.0f;
constexpr float ORBIT_SPEED = 0.5f;

void setCameraOrbitPose(Entity* camera, const float timeSeconds)
{
    if (!camera) {
        return;
    }

    const float phase = timeSeconds * ORBIT_SPEED;
    const float x = ORBIT_RADIUS * std::sin(phase);
    const float z = ORBIT_RADIUS * std::cos(phase);
    const Vector3 position(x, 0.0f, z);
    camera->setLocalPosition(position.getX(), position.getY(), position.getZ());

    const Vector3 lookDir = (Vector3(0.0f, 0.0f, 0.0f) - position).normalized();
    const float pitchDeg = std::asin(std::clamp(lookDir.getY(), -1.0f, 1.0f)) * RAD_TO_DEG;
    const float yawDeg = std::atan2(-lookDir.getX(), -lookDir.getZ()) * RAD_TO_DEG;
    camera->setLocalEulerAngles(pitchDeg, yawDeg, 0.0f);
}

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

const auto otherTex = std::make_unique<Asset>(
    "other",
    AssetType::TEXTURE,
    rootPath + "/textures/seaside-rocks01-height.jpg"
);

const auto glossTex = std::make_unique<Asset>(
    "gloss",
    AssetType::TEXTURE,
    rootPath + "/textures/seaside-rocks01-gloss.jpg"
);

Entity* createCapsuleEntity(Engine* engine, Material* material, const Vector3& position, const float scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale, scale, scale);

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setMaterial(material);
        render->setType("capsule");
    }
    return entity;
}

Texture* requireTexture(const std::unique_ptr<Asset>& asset, const char* label)
{
    const auto resource = asset->resource();
    if (!resource) {
        spdlog::error("Failed to load texture asset '{}'", label);
        return nullptr;
    }
    return std::get<Texture*>(*resource);
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
        "VisuTwin Material Test", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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

    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad texture");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    Texture* other = requireTexture(otherTex, "other");
    Texture* gloss = requireTexture(glossTex, "gloss");
    if (!other || !gloss) {
        shutdown();
        return -1;
    }

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->setLocalPosition(0.0f, 0.7f, 4.0f);
    engine->root()->addChild(camera);

    auto* light = new Entity();
    light->setEngine(engine.get());
    auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>());
    if (lightComponent) {
        lightComponent->setColor(Color(1.0f, 0.8f, 0.25f, 1.0f));
        lightComponent->setIntensity(2.0f);
    }
    light->setLocalEulerAngles(85.0f, -100.0f, 0.0f);
    engine->root()->addChild(light);

    auto materialA = std::make_shared<StandardMaterial>();
    materialA->setName("material-test-a");
    materialA->setBaseColorFactor(Color(0.9f, 0.6f, 0.6f, 1.0f));
    materialA->setMetallicFactor(0.5f);
    materialA->setRoughnessFactor(0.45f);
    // DEVIATION: sheen/sheenGloss maps are not implemented yet.

    auto materialB = std::make_shared<StandardMaterial>();
    materialB->setName("material-test-b");
    materialB->setBaseColorFactor(Color(0.6f, 0.9f, 0.6f, 1.0f));
    materialB->setMetallicFactor(0.8f);
    materialB->setRoughnessFactor(0.4f);
    materialB->setMetallicRoughnessTexture(other);
    materialB->setHasMetallicRoughnessTexture(true);
    // DEVIATION: specular/specularity-factor maps are not implemented yet.

    auto materialC = std::make_shared<StandardMaterial>();
    materialC->setName("material-test-c");
    materialC->setBaseColorFactor(Color(0.6f, 0.6f, 0.9f, 1.0f));
    materialC->setOcclusionTexture(gloss);
    materialC->setHasOcclusionTexture(true);
    materialC->setOcclusionStrength(1.0f);
    // DEVIATION: aoDetailMap (second AO layer) is not implemented yet.

    engine->root()->addChild(createCapsuleEntity(
        engine.get(), materialA.get(), Vector3(-1.0f, 0.0f, 0.0f), 0.7f
    ));
    engine->root()->addChild(createCapsuleEntity(
        engine.get(), materialB.get(), Vector3(1.0f, 0.0f, 0.0f), 0.7f
    ));
    engine->root()->addChild(createCapsuleEntity(
        engine.get(), materialC.get(), Vector3(0.0f, 0.0f, 1.0f), 0.7f
    ));

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float orbitTime = 0.0f;
    bool orbitPaused = false;
    int checkpointIndex = 0;
    constexpr float checkpointTimes[] = {
        0.0f,                // front
        2.09439510239f,      // +120 deg
        4.18879020479f       // +240 deg
    };
    setCameraOrbitPose(camera, checkpointTimes[checkpointIndex]);
    spdlog::info("Parity checkpoints: press 1/2/3 to snap camera; Space to resume orbit");

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_1) {
                checkpointIndex = 0;
                orbitTime = checkpointTimes[checkpointIndex];
                orbitPaused = true;
                setCameraOrbitPose(camera, orbitTime);
                spdlog::info("Checkpoint 1: front view");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_2) {
                checkpointIndex = 1;
                orbitTime = checkpointTimes[checkpointIndex];
                orbitPaused = true;
                setCameraOrbitPose(camera, orbitTime);
                spdlog::info("Checkpoint 2: +120 deg orbit view");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_3) {
                checkpointIndex = 2;
                orbitTime = checkpointTimes[checkpointIndex];
                orbitPaused = true;
                setCameraOrbitPose(camera, orbitTime);
                spdlog::info("Checkpoint 3: +240 deg orbit view");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                orbitPaused = false;
                spdlog::info("Orbit resumed");
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;
        if (!orbitPaused) {
            orbitTime += static_cast<float>(dtSeconds);
            setCameraOrbitPose(camera, orbitTime);
        }
        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
