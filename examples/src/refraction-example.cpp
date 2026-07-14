// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Dynamic grab-pass refraction demo: a glass sphere refracts the mid-frame scene
// color grab, distorting the colorful column backdrop behind it. Auto-cycles between
// dynamic (grab-pass) refraction and the env-atlas fallback every few seconds
// (1 = dynamic, 2 = env atlas, Space = auto-cycle).
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <QuartzCore/QuartzCore.hpp>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// The helipad environment atlas gives the env-atlas refraction/reflection path real
// content to sample (mirrors the PlayCanvas material-refraction example).
const auto helipad = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

// Seaside-rocks PBR maps texture the backdrop so refraction shows a realistic rocky
// background rather than flat synthetic color.
const auto rocksColor = std::make_unique<Asset>(
    "rocks-color", AssetType::TEXTURE, rootPath + "/textures/seaside-rocks01-color.jpg");
const auto rocksNormal = std::make_unique<Asset>(
    "rocks-normal", AssetType::TEXTURE, rootPath + "/textures/seaside-rocks01-normal.jpg");
const auto rocksGloss = std::make_unique<Asset>(
    "rocks-gloss", AssetType::TEXTURE, rootPath + "/textures/seaside-rocks01-gloss.jpg");

Texture* requireTexture(const std::unique_ptr<Asset>& asset, const char* label)
{
    const auto resource = asset->resource();
    if (!resource) {
        spdlog::error("Failed to load texture asset '{}'", label);
        return nullptr;
    }
    return std::get<Texture*>(*resource);
}

Entity* createEntity(Engine* engine, Material* material, const char* type,
    const Vector3& position, const Vector3& scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setMaterial(material);
        render->setType(type);
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
        "VisuTwin Dynamic Refraction", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.25f, 0.25f, 0.28f);

    // Give the env-atlas refraction path real content to sample.
    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad texture");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    Texture* rocksColorTex = requireTexture(rocksColor, "rocks-color");
    Texture* rocksNormalTex = requireTexture(rocksNormal, "rocks-normal");
    Texture* rocksGlossTex = requireTexture(rocksGloss, "rocks-gloss");
    if (!rocksColorTex || !rocksNormalTex || !rocksGlossTex) {
        shutdown();
        return -1;
    }

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->setLocalPosition(0.0f, 1.2f, 6.0f);
    camera->setLocalEulerAngles(-4.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    // The dynamic-refraction grab pass only runs when the camera publishes the
    // scene color map.
    if (cameraComponent) {
        cameraComponent->requestSceneColorMap(true);
    }

    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lightComponent->setIntensity(1.4f);
    }
    light->setLocalEulerAngles(50.0f, -30.0f, 0.0f);
    engine->root()->addChild(light);

    // Colorful column backdrop — sharp vertical color edges make the refraction
    // distortion unmistakable.
    const Color columnColors[6] = {
        Color(0.9f, 0.15f, 0.15f, 1.0f), Color(0.95f, 0.6f, 0.1f, 1.0f),
        Color(0.9f, 0.85f, 0.1f, 1.0f), Color(0.15f, 0.7f, 0.25f, 1.0f),
        Color(0.15f, 0.4f, 0.9f, 1.0f), Color(0.6f, 0.2f, 0.8f, 1.0f)
    };
    std::vector<std::shared_ptr<StandardMaterial>> columnMaterials;
    for (int i = 0; i < 6; ++i) {
        auto material = std::make_shared<StandardMaterial>();
        material->setName("column-" + std::to_string(i));
        // Tinted seaside-rocks: the color map keeps a rocky backdrop while the per-column
        // tint preserves sharp color edges that make the refraction distortion obvious.
        material->setDiffuse(columnColors[i]);
        material->setDiffuseMap(rocksColorTex);
        material->setNormalMap(rocksNormalTex);
        material->setBumpiness(1.0f);
        material->setGlossMap(rocksGlossTex);
        material->setMetalness(0.0f);
        material->setGloss(0.4f);
        columnMaterials.push_back(material);
        engine->root()->addChild(createEntity(
            engine.get(), material.get(), "box",
            Vector3(-3.75f + 1.5f * static_cast<float>(i), 1.0f, -2.5f), Vector3(1.2f, 4.0f, 0.6f)
        ));
    }

    // Rocky floor (seaside-rocks color+normal+gloss), like the PlayCanvas ground.
    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setName("floor");
    floorMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
    floorMaterial->setDiffuseMap(rocksColorTex);
    floorMaterial->setNormalMap(rocksNormalTex);
    floorMaterial->setBumpiness(1.0f);
    floorMaterial->setGlossMap(rocksGlossTex);
    floorMaterial->setMetalness(0.0f);
    floorMaterial->setGloss(0.4f);
    engine->root()->addChild(createEntity(
        engine.get(), floorMaterial.get(), "plane", Vector3(0.0f, -1.0f, 0.0f), Vector3(24.0f, 1.0f, 24.0f)
    ));

    // The glass sphere: transparent so it renders after the depth-layer grab.
    auto glassMaterial = std::make_shared<StandardMaterial>();
    glassMaterial->setName("glass");
    glassMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
    glassMaterial->setMetalness(0.0f);
    glassMaterial->setGlossInvert(true);
    glassMaterial->setGloss(0.05f);  // near-mirror smooth
    glassMaterial->setTransmissionFactor(1.0f);
    glassMaterial->setRefractionIndex(1.5f);
    glassMaterial->setThickness(0.8f);
    glassMaterial->setUseDynamicRefraction(true);
    glassMaterial->setTransparent(true);
    auto* glass = createEntity(
        engine.get(), glassMaterial.get(), "sphere", Vector3(0.0f, 1.0f, 0.8f), Vector3(2.4f, 2.4f, 2.4f)
    );
    engine->root()->addChild(glass);

    spdlog::info("Dynamic refraction: glass sphere over colorful columns");
    spdlog::info("Keys: 1 = dynamic, 2 = +dispersion, 3 = +volume attenuation, 4 = env atlas, Space = auto-cycle, Esc = quit");

    bool running = true;
    bool autoCycle = true;
    int phase = 0;
    float cycleTimer = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    // Phases: 0 = dynamic grab, 1 = dynamic + dispersion (KHR_materials_dispersion),
    // 2 = dynamic + Beer-law volume attenuation (KHR_materials_volume), 3 = env atlas.
    const auto applyMode = [&](const int newPhase) {
        phase = newPhase;
        glassMaterial->setUseDynamicRefraction(phase != 3);
        glassMaterial->setDispersion(phase == 1 ? 10.0f : 0.0f);
        if (phase == 2) {
            glassMaterial->setAttenuationColor(Color(0.9f, 0.3f, 0.1f, 1.0f));
            glassMaterial->setAttenuationDistance(0.35f);
        } else {
            glassMaterial->setAttenuationDistance(0.0f);
        }
        static const char* names[4] = {
            "dynamic grab pass", "dynamic + dispersion",
            "dynamic + volume attenuation (orange Beer-law)", "env atlas"};
        spdlog::info("Refraction mode: {}", names[phase]);
    };
    applyMode(0);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key >= SDLK_1 && event.key.key <= SDLK_4) {
                autoCycle = false;
                applyMode(static_cast<int>(event.key.key - SDLK_1));
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                autoCycle = true;
                cycleTimer = 0.0f;
                spdlog::info("Auto-cycle resumed");
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (autoCycle) {
            cycleTimer += static_cast<float>(dtSeconds);
            if (cycleTimer >= 3.0f) {
                cycleTimer = 0.0f;
                applyMode((phase + 1) % 4);
            }
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
