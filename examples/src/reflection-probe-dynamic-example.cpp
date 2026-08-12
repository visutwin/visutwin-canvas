// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Dynamic reflection-probe demo: a chrome sphere reflects a ring of orbiting
// emissive boxes via a runtime scene-capture cubemap probe. Each frame the
// ReflectionProbe renders the scene into six cube faces from the sphere centre
// and installs it as the scene reflection probe, so the reflection tracks the
// moving boxes live (unlike a static, hand-authored cubemap). The sphere sits
// on its own layer excluded from the probe capture, so it does not reflect
// itself. Esc quits.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <cmath>
#include <memory>
#include <SDL3/SDL.h>

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
#include "framework/extras/reflectionProbe.h"
#include "framework/constants.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/layer.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;
constexpr int LAYERID_REFLECTIVE = 50;

using namespace visutwin::canvas;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;

const std::string rootPath = ASSET_DIR;

// Helipad environment atlas — the skybox/IBL backdrop the chrome sphere reflects
// (matching the PlayCanvas reflection-cubemap example). Loaded as an RGBP-packed
// equirect environment atlas.
const auto helipad = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

Entity* createEntity(Engine* engine, Material* material, const char* type,
    const Vector3& position, const Vector3& scale, const std::vector<int>& layers)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
    if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
        render->setMaterial(material);
        render->setType(type);
        render->setLayers(layers);
    }
    return entity;
}

int main()
{
    log::init();
    log::set_level_debug();

    const auto shutdown = []() {
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        SDL_Quit();
    };

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);
    window = SDL_CreateWindow("Dynamic Reflection Probe", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    if (!window) { shutdown(); return -1; }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { shutdown(); return -1; }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) { shutdown(); return -1; }

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) { shutdown(); return -1; }

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
    scene->setAmbientLight(0.05f, 0.05f, 0.06f);

    // Helipad environment atlas + skybox backdrop (matches the PlayCanvas
    // reflection-cubemap example: setSkyboxMip(0), setSkyboxIntensity(2.0)).
    // Installed BEFORE the render loop so the skybox is present when the dynamic
    // probe captures its cube faces — the chrome sphere then reflects BOTH the
    // captured orbiting boxes AND the environment.
    scene->setSkyboxMip(0);
    scene->setSkyboxIntensity(2.0f);
    if (const auto helipadResource = helipad->resource()) {
        scene->setEnvAtlas(std::get<Texture*>(*helipadResource));
    } else {
        spdlog::error("Failed to load helipad env atlas texture");
    }

    // Layer composition: WORLD (orbiting boxes + floor), a dedicated REFLECTIVE
    // layer for the chrome sphere (so the probe does not capture it), SKYBOX.
    const auto defaultComposition = scene->layers();
    const auto worldLayer = defaultComposition->getLayerById(LAYERID_WORLD);
    const auto skyboxLayer = defaultComposition->getLayerById(LAYERID_SKYBOX);
    auto reflectiveLayer = std::make_shared<Layer>("Reflective", LAYERID_REFLECTIVE);
    auto composition = std::make_shared<LayerComposition>("reflection-probe-composition");
    composition->pushOpaque(worldLayer);
    composition->pushOpaque(reflectiveLayer);
    if (skyboxLayer) {
        composition->pushOpaque(skyboxLayer);
    }
    scene->setLayers(composition);

    // Chrome sphere on the REFLECTIVE layer (excluded from probe capture).
    auto chrome = std::make_shared<StandardMaterial>();
    chrome->setDiffuse(Color(0.95f, 0.95f, 0.95f, 1.0f));
    chrome->setMetalness(1.0f);
    chrome->setGlossInvert(true);
    chrome->setGloss(0.02f);
    engine->root()->addChild(createEntity(engine.get(), chrome.get(), "sphere",
        Vector3(0.0f, 1.2f, 0.0f), Vector3(2.4f, 2.4f, 2.4f), {LAYERID_REFLECTIVE}));

    // A muted floor (WORLD) so the probe/sphere have a ground below them.
    auto floorMat = std::make_shared<StandardMaterial>();
    floorMat->setDiffuse(Color(0.10f, 0.11f, 0.13f, 1.0f));
    floorMat->setMetalness(0.0f);
    floorMat->setGlossInvert(true);
    floorMat->setGloss(0.6f);
    engine->root()->addChild(createEntity(engine.get(), floorMat.get(), "plane",
        Vector3(0.0f, 0.0f, 0.0f), Vector3(30.0f, 1.0f, 30.0f), {LAYERID_WORLD}));

    // Ring of emissive colored boxes (WORLD) — the content the sphere reflects.
    constexpr int BOX_COUNT = 5;
    const Color boxColors[BOX_COUNT] = {
        Color(1.0f, 0.15f, 0.12f, 1.0f), Color(0.15f, 1.0f, 0.25f, 1.0f),
        Color(0.2f, 0.4f, 1.0f, 1.0f), Color(1.0f, 0.85f, 0.15f, 1.0f),
        Color(0.9f, 0.2f, 1.0f, 1.0f)};
    std::vector<std::shared_ptr<StandardMaterial>> boxMats;
    std::vector<Entity*> boxes;
    for (int i = 0; i < BOX_COUNT; ++i) {
        auto m = std::make_shared<StandardMaterial>();
        m->setDiffuse(Color(0.02f, 0.02f, 0.02f, 1.0f));
        m->setEmissive(boxColors[i]);
        m->setEmissiveIntensity(3.0f);
        boxMats.push_back(m);
        auto* box = createEntity(engine.get(), m.get(), "box",
            Vector3(0.0f, 1.2f, 0.0f), Vector3(0.9f, 0.9f, 0.9f), {LAYERID_WORLD});
        engine->root()->addChild(box);
        boxes.push_back(box);
    }

    // Dynamic reflection probe — constructed BEFORE the main camera so its six
    // face cameras render first each frame. Captures WORLD + SKYBOX (not the
    // sphere's REFLECTIVE layer).
    auto probe = std::make_unique<ReflectionProbe>(engine.get(), 128);
    probe->setPosition(Vector3(0.0f, 1.2f, 0.0f));
    probe->setBox(Vector3(-8.0f, 0.0f, -8.0f), Vector3(8.0f, 8.0f, 8.0f), true);
    probe->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
    probe->setNearFar(0.1f, 100.0f);
    probe->setDynamic(true);

    // Main camera (presentation) — renders all three layers.
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    cameraComponent->setLayers({LAYERID_WORLD, LAYERID_REFLECTIVE, LAYERID_SKYBOX});
    camera->setLocalPosition(0.0f, 2.6f, 8.5f);
    camera->setLocalEulerAngles(-10.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 0.98f, 0.95f, 1.0f));
        lightComponent->setIntensity(1.0f);
    }
    light->setLocalEulerAngles(55.0f, -30.0f, 0.0f);
    engine->root()->addChild(light);

    spdlog::info("Dynamic reflection probe: chrome sphere reflects orbiting emissive boxes (live capture). Esc quits.");

    bool running = true;
    float elapsed = 0.0f;
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

        // Orbit the emissive boxes around the sphere so the reflection moves.
        for (int i = 0; i < BOX_COUNT; ++i) {
            const float theta = elapsed * 0.7f + static_cast<float>(i) * (2.0f * 3.14159265f / BOX_COUNT);
            const float radius = 4.2f;
            boxes[i]->setLocalPosition(std::cos(theta) * radius, 1.2f + std::sin(theta * 1.3f) * 0.9f,
                std::sin(theta) * radius);
        }

        // Slow camera orbit so the parallax reads as 3D.
        const float camAngle = std::sin(elapsed * 0.2f) * 0.6f;
        camera->setLocalPosition(std::sin(camAngle) * 8.7f, 2.6f, std::cos(camAngle) * 8.7f);
        camera->setLocalEulerAngles(-10.0f, camAngle * 57.2958f, 0.0f);

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
        probe->update();   // regenerate cube mips + keep the probe installed
    }

    probe.reset();
    shutdown();
    return 0;
}
