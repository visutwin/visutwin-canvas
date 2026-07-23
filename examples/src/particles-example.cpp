// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// GPU particle system demo: three emitters on one campfire scene —
//   fire   : sphere-shape additive emitter, orange->red color graph, grow/shrink scale
//   smoke  : box-shape normal-blend emitter, slow gray puffs with rotation
//   sparks : additive emitter with gravity + velocity spread and tiny fast particles
// Space pauses/resumes all systems, R resets them, Esc quits.
//
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#endif

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>
#include <string>

#ifdef VISUTWIN_HAS_METAL
#include <QuartzCore/QuartzCore.hpp>
#endif

#include <framework/assets/asset.h>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
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

// Real particle sprites (mirrors PlayCanvas' particles-spark / particles-snow
// examples, which attach spark.png / snowflake.png as the emitter color map).
// spark.png  — bright streak sprite for the fast embers (sparks emitter).
// snowflake.png — soft radial puff used for the fire and smoke billboards.
const auto sparkTexAsset = std::make_unique<Asset>(
    "spark",
    AssetType::TEXTURE,
    rootPath + "/textures/spark.png"
);

const auto softPuffTexAsset = std::make_unique<Asset>(
    "snowflake",
    AssetType::TEXTURE,
    rootPath + "/textures/snowflake.png"
);

Texture* loadSpriteTexture(const std::unique_ptr<Asset>& asset, const char* label)
{
    const auto resource = asset->resource();
    if (!resource) {
        spdlog::error("Failed to load particle sprite '{}'", label);
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
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        SDL_Quit();
    };

#ifdef VISUTWIN_HAS_METAL
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
#endif
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin GPU Particles", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
#ifdef VISUTWIN_HAS_VULKAN
        | SDL_WINDOW_VULKAN
#endif
    );
    if (!window) { shutdown(); return -1; }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { shutdown(); return -1; }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    void* swapchain = nullptr;
#ifdef VISUTWIN_HAS_METAL
    swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) { shutdown(); return -1; }
#endif

    GraphicsDeviceOptions deviceOptions;
#ifdef VISUTWIN_HAS_VULKAN
    deviceOptions.backend = Backend::Vulkan;
#endif
    deviceOptions.swapChain = swapchain;
    deviceOptions.window = window;
    auto device = createGraphicsDevice(deviceOptions);
    if (!device) { shutdown(); return -1; }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ParticleSystemComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.06f, 0.06f, 0.09f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    if (auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>())) {
        cameraComponent->camera()->setClearColor(Color(0.02f, 0.02f, 0.04f, 1.0f));
    }
    camera->setLocalPosition(0.0f, 1.8f, 6.5f);
    camera->setLocalEulerAngles(-10.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(0.5f, 0.6f, 0.9f, 1.0f));
        lightComponent->setIntensity(0.35f);  // dim moonlight
    }
    light->setLocalEulerAngles(50.0f, -30.0f, 0.0f);
    engine->root()->addChild(light);

    // Dark ground.
    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setDiffuse(Color(0.16f, 0.15f, 0.17f, 1.0f));
    floorMaterial->setGlossInvert(true);
    floorMaterial->setGloss(0.9f);
    {
        auto* floor = new Entity();
        floor->setEngine(engine.get());
        floor->setLocalScale(30.0f, 1.0f, 30.0f);
        if (auto* render = static_cast<RenderComponent*>(floor->addComponent<RenderComponent>())) {
            render->setMaterial(floorMaterial.get());
            render->setType("plane");
        }
        engine->root()->addChild(floor);
    }

    // Load the sprite textures now that the graphics device is live.
    Texture* sparkTex = loadSpriteTexture(sparkTexAsset, "spark.png");
    Texture* softPuffTex = loadSpriteTexture(softPuffTexAsset, "snowflake.png");

    // ── Fire: additive sphere emitter with orange->deep-red gradient ─────────
    auto* fireEntity = new Entity();
    fireEntity->setEngine(engine.get());
    fireEntity->setLocalPosition(0.0f, 0.15f, 0.0f);
    auto* fire = static_cast<ParticleSystemComponent*>(fireEntity->addComponent<ParticleSystemComponent>());
    if (fire) {
        auto& o = fire->options();
        o.numParticles = 600;
        o.lifetime = 0.9f;
        o.lifetime2 = 1.4f;
        o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
        o.emitterRadius = 0.25f;
        o.initialVelocity = Vector3(0.0f, 1.6f, 0.0f);
        o.velocitySpread = Vector3(0.35f, 0.4f, 0.35f);
        o.damping = 0.4f;
        o.rotationSpeed = -90.0f;
        o.rotationSpeed2 = 90.0f;
        o.blendType = ParticleBlendType::BLEND_ADDITIVE;
        o.intensity = 1.5f;
        o.scaleGraph = Curve();
        o.scaleGraph.add(0.0f, 0.12f);
        o.scaleGraph.add(0.25f, 0.5f);
        o.scaleGraph.add(1.0f, 0.05f);
        o.colorGraph.curves[0] = Curve({0.0f, 1.0f, 0.5f, 1.0f, 1.0f, 0.7f});   // r
        o.colorGraph.curves[1] = Curve({0.0f, 0.85f, 0.5f, 0.35f, 1.0f, 0.05f}); // g
        o.colorGraph.curves[2] = Curve({0.0f, 0.35f, 0.5f, 0.05f, 1.0f, 0.0f});  // b
        o.alphaGraph = Curve({0.0f, 0.0f, 0.1f, 0.55f, 1.0f, 0.0f});
        o.colorMap = softPuffTex;  // soft radial puff sprite for the flames
        fire->apply();
    }
    engine->root()->addChild(fireEntity);

    // ── Smoke: normal-blend gray puffs, slow rise + rotation ─────────────────
    auto* smokeEntity = new Entity();
    smokeEntity->setEngine(engine.get());
    smokeEntity->setLocalPosition(0.0f, 1.0f, 0.0f);
    auto* smoke = static_cast<ParticleSystemComponent*>(smokeEntity->addComponent<ParticleSystemComponent>());
    if (smoke) {
        auto& o = smoke->options();
        o.numParticles = 120;
        o.lifetime = 3.0f;
        o.lifetime2 = 4.5f;
        o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_BOX;
        o.emitterExtents = Vector3(0.2f, 0.1f, 0.2f);
        o.initialVelocity = Vector3(0.15f, 0.9f, 0.0f);   // slight wind
        o.velocitySpread = Vector3(0.2f, 0.2f, 0.2f);
        o.damping = 0.15f;
        o.startAngle = -180.0f;
        o.startAngle2 = 180.0f;
        o.rotationSpeed = -25.0f;
        o.rotationSpeed2 = 25.0f;
        o.blendType = ParticleBlendType::BLEND_NORMAL;
        o.intensity = 0.5f;
        o.scaleGraph = Curve();
        o.scaleGraph.add(0.0f, 0.3f);
        o.scaleGraph.add(1.0f, 1.6f);
        o.colorGraph.curves[0] = Curve({0.0f, 0.45f, 1.0f, 0.3f});
        o.colorGraph.curves[1] = Curve({0.0f, 0.45f, 1.0f, 0.3f});
        o.colorGraph.curves[2] = Curve({0.0f, 0.48f, 1.0f, 0.33f});
        o.alphaGraph = Curve({0.0f, 0.0f, 0.15f, 0.45f, 1.0f, 0.0f});
        o.colorMap = softPuffTex;  // same soft puff sprite reads as smoke wisps
        smoke->apply();
    }
    engine->root()->addChild(smokeEntity);

    // ── Sparks: tiny fast additive embers under gravity ──────────────────────
    auto* sparkEntity = new Entity();
    sparkEntity->setEngine(engine.get());
    sparkEntity->setLocalPosition(0.0f, 0.3f, 0.0f);
    auto* sparks = static_cast<ParticleSystemComponent*>(sparkEntity->addComponent<ParticleSystemComponent>());
    if (sparks) {
        auto& o = sparks->options();
        o.numParticles = 200;
        o.lifetime = 0.8f;
        o.lifetime2 = 1.6f;
        o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
        o.emitterRadius = 0.1f;
        o.initialVelocity = Vector3(0.0f, 3.2f, 0.0f);
        o.velocitySpread = Vector3(1.6f, 1.0f, 1.6f);
        o.gravity = Vector3(0.0f, -5.0f, 0.0f);
        o.blendType = ParticleBlendType::BLEND_ADDITIVE;
        o.intensity = 3.5f;
        o.scaleGraph = Curve();
        o.scaleGraph.add(0.0f, 0.035f);
        o.scaleGraph.add(1.0f, 0.015f);
        o.colorGraph.curves[0] = Curve({0.0f, 1.0f});
        o.colorGraph.curves[1] = Curve({0.0f, 0.75f, 1.0f, 0.3f});
        o.colorGraph.curves[2] = Curve({0.0f, 0.25f, 1.0f, 0.0f});
        o.alphaGraph = Curve({0.0f, 1.0f, 0.8f, 1.0f, 1.0f, 0.0f});
        o.colorMap = sparkTex;  // bright streak sprite (PlayCanvas particles-spark)
        sparks->apply();
    }
    engine->root()->addChild(sparkEntity);

    spdlog::info("GPU particles: fire (600, additive) + smoke (120, normal) + sparks (200, gravity)");
    spdlog::info("Keys: Space = pause/resume, R = reset, Esc = quit");

    bool running = true;
    bool paused = false;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                paused = !paused;
                for (auto* c : {fire, smoke, sparks}) {
                    if (c) { paused ? c->pause() : c->play(); }
                }
                spdlog::info("Particles {}", paused ? "paused" : "playing");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R) {
                for (auto* c : {fire, smoke, sparks}) {
                    if (c) c->reset();
                }
                spdlog::info("Particles reset");
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
