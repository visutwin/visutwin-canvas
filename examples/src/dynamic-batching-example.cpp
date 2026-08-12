// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Dynamic batching example — port of PlayCanvas graphics/batching-dynamic.
//
// Creates several hundred small procedural primitives that share a pair of
// materials and are all tagged into a SINGLE dynamic BatchGroup. The engine
// merges them into a handful of batched draw calls (one per material) instead
// of hundreds of individual draws. Because the group is DYNAMIC, every object
// can be moved every frame — the BatchManager rebuilds the per-frame matrix
// palette in Engine::update() (BatchManager::updateAll()).
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <iostream>
#include <cmath>
#include <random>
#include <vector>

#include <framework/assets/asset.h>

#include <QuartzCore/QuartzCore.hpp>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/constants.h"
#include "framework/batching/batchManager.h"
#include "framework/batching/batchGroup.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1100;
constexpr int WINDOW_HEIGHT = 750;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Optional image-based lighting so the metallic primitives get nice reflections.
const auto envAtlas = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

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

    spdlog::info("*** Dynamic Batching Example Started ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Dynamic Batching Example", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    createOptions.registerComponentSystem<LightComponentSystem>();
    // Note: AppOptions::batchManager left unset — Engine::init() creates a
    // default BatchManager, reachable via engine->batcher().

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);

    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);

    engine->start();

    auto scene = engine->scene();
    scene->setSkyboxMip(2);
    scene->setExposure(1.2f);
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.25f, 0.28f, 0.35f);

    const auto envAtlasResource = envAtlas->resource();
    if (envAtlasResource && std::holds_alternative<Texture*>(*envAtlasResource)) {
        scene->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
    } else {
        spdlog::warn("Environment atlas failed to load — continuing without IBL");
    }

    // -----------------------------------------------------------------------
    // Two shared materials — batching produces (at least) one draw call per
    // distinct material, so this scene resolves to ~2 batched draws.
    // -----------------------------------------------------------------------
    auto* material1 = new StandardMaterial();
    material1->setDiffuse(Color(1.0f, 1.0f, 0.0f));
    material1->setGloss(0.4f);
    material1->setMetalness(0.5f);
    material1->setUseMetalness(true);

    auto* material2 = new StandardMaterial();
    material2->setDiffuse(Color(0.0f, 1.0f, 1.0f));
    material2->setGloss(0.4f);
    material2->setMetalness(0.5f);
    material2->setUseMetalness(true);

    // -----------------------------------------------------------------------
    // Register a single DYNAMIC BatchGroup BEFORE creating the entities.
    // BatchGroup fields set: id, name, dynamic=true, maxAabbSize=100,
    // layers={} (empty → BatchManager defaults the batch to WORLD layer id 1,
    // which is what RenderComponent uses by default).
    // -----------------------------------------------------------------------
    constexpr int kBatchGroupId = 0;
    BatchGroup meshesGroup(kBatchGroupId, "Meshes", /*dynamic*/ true,
                           /*maxAabbSize*/ 100.0f, /*layers*/ {});
    engine->batcher()->addGroup(meshesGroup);

    // -----------------------------------------------------------------------
    // Create many small procedural primitives, all tagged into the batch group.
    // -----------------------------------------------------------------------
    constexpr int numInstances = 500;
    const std::vector<std::string> shapes = {"box", "cone", "cylinder", "sphere", "capsule"};

    std::mt19937 rng(1337u);
    std::uniform_int_distribution<int> shapeDist(0, static_cast<int>(shapes.size()) - 1);
    std::uniform_real_distribution<float> matDist(0.0f, 1.0f);

    std::vector<Entity*> entities;
    entities.reserve(numInstances);

    for (int i = 0; i < numInstances; ++i) {
        const std::string& shapeName = shapes[shapeDist(rng)];

        auto* entity = new Entity();
        entity->setEngine(engine.get());
        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        if (render) {
            render->setType(shapeName);
            render->setMaterial(matDist(rng) < 0.5f ? material1 : material2);
            render->setCastShadows(true);

            // Tag into the batch group — the engine will try to render all of
            // these in a small number of draw calls (one per material).
            render->setBatchGroupId(kBatchGroupId);
        }
        entity->setLocalScale(0.6f, 0.6f, 0.6f);
        engine->root()->addChild(entity);
        entities.push_back(entity);
    }

    // -----------------------------------------------------------------------
    // Ground box — NOT batched (its own draw), gives the moving meshes context.
    // -----------------------------------------------------------------------
    {
        auto* ground = new Entity();
        ground->setEngine(engine.get());
        auto* groundRender = static_cast<RenderComponent*>(ground->addComponent<RenderComponent>());
        if (groundRender) {
            groundRender->setType("box");
            groundRender->setMaterial(material2);
            groundRender->setReceiveShadows(true);
        }
        ground->setLocalScale(150.0f, 1.0f, 150.0f);
        ground->setLocalPosition(0.0f, -26.0f, 0.0f);
        engine->root()->addChild(ground);
    }

    // -----------------------------------------------------------------------
    // Camera on an orbiting pivot. The camera child looks at the pivot origin;
    // rotating the pivot each frame orbits the whole scene.
    // -----------------------------------------------------------------------
    auto* cameraPivot = new Entity();
    cameraPivot->setEngine(engine.get());
    engine->root()->addChild(cameraPivot);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
        cameraComp->camera()->setFarClip(500.0f);
        auto rendering = cameraComp->rendering();
        rendering.toneMapping = TONEMAP_ACES;
        cameraComp->setRendering(rendering);
    }
    constexpr float kCamHeight = 15.0f;
    constexpr float kCamDist = 70.0f;
    camera->setLocalPosition(0.0f, kCamHeight, kCamDist);
    // Pitch down to keep the origin centred (camera looks down local -Z).
    const float pitchDeg = -std::atan2(kCamHeight, kCamDist) * 180.0f / static_cast<float>(M_PI);
    camera->setLocalEulerAngles(pitchDeg, 0.0f, 0.0f);
    cameraPivot->addChild(camera);

    // Directional light parented to the camera so it rotates with the view.
    auto* light = new Entity();
    light->setEngine(engine.get());
    auto* lightComp = static_cast<LightComponent*>(light->addComponent<LightComponent>());
    if (lightComp) {
        lightComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        lightComp->setColor(Color(1.0f, 0.95f, 0.85f));
        lightComp->setIntensity(1.2f);
        lightComp->setCastShadows(true);
        lightComp->setShadowBias(0.2f);
        lightComp->setShadowNormalBias(0.06f);
        lightComp->setShadowDistance(150.0f);
    }
    light->setLocalEulerAngles(15.0f, 30.0f, 0.0f);
    camera->addChild(light);

    // -----------------------------------------------------------------------
    // Build the batches. prepare() merges geometry per (group, material),
    // hides the originals, and registers the batched MeshInstances with the
    // scene layers. Must run AFTER the tagged entities exist.
    // -----------------------------------------------------------------------
    engine->batcher()->prepare(scene.get());

    spdlog::info("Dynamic batching: BatchGroup id={} name=\"{}\" dynamic={} maxAabbSize={} — "
                 "{} procedural primitives sharing 2 materials tagged into it.",
                 kBatchGroupId, "Meshes", true, 100.0f, numInstances);
    spdlog::info("These render as a handful of batched draws (one per material) rebuilt every "
                 "frame via BatchManager::updateAll().");

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
        const double dtSeconds =
            static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;
        const float dt = static_cast<float>(dtSeconds);
        time += dt;

        // Move every batched entity along its own orbit — exercises DYNAMIC
        // batching (transforms change per frame, palette rebuilt in updateAll).
        for (int i = 0; i < numInstances; ++i) {
            const float radius = 5.0f + (20.0f * static_cast<float>(i)) / numInstances;
            const float speed = static_cast<float>(i) / numInstances;
            const float fi = static_cast<float>(i);
            entities[i]->setLocalPosition(
                radius * std::sin(fi + time * speed),
                radius * std::cos(fi + time * speed),
                radius * std::cos(fi + 2.0f * time * speed)
            );
            // A little per-object spin for extra visible motion.
            entities[i]->setLocalEulerAngles(time * 20.0f * speed, fi * 7.0f, 0.0f);
        }

        // Orbit the camera pivot around the scene.
        cameraPivot->setLocalEulerAngles(0.0f, time * 15.0f, 0.0f);

        engine->update(dt);   // BatchManager::updateAll() runs inside here.
        engine->render();
    }

    shutdown();

    spdlog::info("*** Dynamic Batching Example Finished ***");

    return 0;
}
