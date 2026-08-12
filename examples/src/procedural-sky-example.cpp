// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Procedural sky (Nishita single-scattering atmosphere) demo — port in spirit of
// PlayCanvas graphics/procedural-sky. The scene renders the physically-based
// atmosphere as the sky (SKYTYPE_ATMOSPHERE + Scene::setAtmosphereEnabled), a
// statue + procedural ground/boxes lit by a single directional "sun" light, and
// an orbit camera. The sun direction is animated over time (a sunrise -> noon ->
// sunset sweep); on every frame BOTH the atmosphere sunDirection uniform AND the
// directional light's orientation are updated from the same quaternion, so the
// sky and the ground lighting always agree.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>

#include <QuartzCore/QuartzCore.hpp>

#include <core/math/color.h>
#include <core/math/quaternion.h>
#include <core/math/vector3.h>
#include <core/shape/boundingBox.h>
#include <framework/assets/asset.h>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/constants.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "platform/graphics/graphicsDeviceCreate.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

const auto statue = std::make_unique<Asset>(
    "statue",
    AssetType::CONTAINER,
    rootPath + "/models/statue.glb"
);

// GPU-side atmosphere uniform block. MUST be a byte-exact mirror of
// UniformBinder::AtmosphereUniforms (6 x float4 = 96 bytes) — a mis-sized or
// re-ordered field silently corrupts the shader read and produces a black/broken
// sky. Field order + default values copied verbatim from uniformBinder.h.
struct alignas(16) AtmosphereData
{
    float planetCenterAndRadius[4]          = {0.0f, 0.0f, 0.0f, 6371000.0f};
    float atmosphereRadiusAndSunIntensity[4] = {6471000.0f, 22.0f, 0.9998f, 0.0f};
    float rayleighCoeffAndScaleHeight[4]     = {5.5e-6f, 13.0e-6f, 22.4e-6f, 8500.0f};
    float mieCoeffAndScaleHeight[4]          = {21.0e-6f, 1200.0f, 0.758f, 0.0f};
    float sunDirection[4]                    = {0.0f, 1.0f, 0.0f, 0.0f};
    float cameraAltitudeAndParams[4]         = {0.0f, 32.0f, 8.0f, 0.0f};
};
static_assert(sizeof(AtmosphereData) == 96, "AtmosphereData must be 96 bytes (6 x float4)");

Entity* createPrimitive(Engine* engine, const char* type, Material* material,
    const Vector3& position, const Vector3& scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
    if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
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
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        SDL_Quit();
    };

    spdlog::info("*** Visualization Engine Started *** ");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Procedural Sky (Nishita Atmosphere)", WINDOW_WIDTH, WINDOW_HEIGHT,
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
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);

    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);

    engine->start();

    auto scene = engine->scene();
    // A modest ambient fill (bounced sky light approximation) so shadowed sides
    // don't go fully black — the directional sun provides the key light.
    scene->setAmbientLight(0.20f, 0.22f, 0.28f);
    scene->setExposure(1.0f);

    // --- Enable the procedural Nishita atmosphere as the sky ---
    scene->setSkyType(SKYTYPE_ATMOSPHERE);
    scene->setAtmosphereEnabled(true);

    AtmosphereData atmo; // starts with the physically-based defaults above

    // --- Sun: a single directional light kept in sync with the atmosphere ---
    auto* sunEntity = new Entity();
    sunEntity->setEngine(engine.get());
    auto* sun = static_cast<LightComponent*>(sunEntity->addComponent<LightComponent>());
    if (sun) {
        sun->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        sun->setColor(Color(1.0f, 0.96f, 0.88f));
        sun->setIntensity(4.0f);
        sun->setCastShadows(true);
    }
    engine->root()->addChild(sunEntity);

    // Orient the sun for a given elevation (deg above horizon) + azimuth (deg).
    // The light's world +Y column IS the direction toward the sun (LightComponent
    // ::direction() returns -Y). We derive the sun vector from the SAME quaternion
    // we assign to the light, so the sky's sunDirection and the ground key light
    // are guaranteed to match regardless of the euler-order convention.
    const auto applySun = [&](float elevationDeg, float azimuthDeg) {
        // pitch tilts local +Y from straight up (elevation 90) down toward the
        // horizon (elevation 0); yaw sweeps the sun around the compass.
        const float pitch = 90.0f - elevationDeg;
        const Quaternion q = Quaternion::fromEulerAngles(pitch, azimuthDeg, 0.0f);
        sunEntity->setLocalRotation(q);

        const Vector3 sunDir = q * Vector3(0.0f, 1.0f, 0.0f); // == light "up" == toward sun
        atmo.sunDirection[0] = sunDir.getX();
        atmo.sunDirection[1] = sunDir.getY();
        atmo.sunDirection[2] = sunDir.getZ();
        atmo.sunDirection[3] = 0.0f;
        scene->setAtmosphereUniforms(&atmo, sizeof(atmo));
    };

    // Start low in the east for a warm sunrise horizon.
    applySun(8.0f, -70.0f);

    // --- Ground + a few primitives + the statue to receive the sun ---
    auto groundMaterial = std::make_shared<StandardMaterial>();
    groundMaterial->setDiffuse(Color(0.35f, 0.33f, 0.30f));
    groundMaterial->setGloss(0.15f);
    groundMaterial->setMetalness(0.0f);
    engine->root()->addChild(createPrimitive(engine.get(), "plane", groundMaterial.get(),
        Vector3(0.0f, -0.5f, 0.0f), Vector3(60.0f, 1.0f, 60.0f)));

    auto boxMaterial = std::make_shared<StandardMaterial>();
    boxMaterial->setDiffuse(Color(0.55f, 0.35f, 0.25f));
    boxMaterial->setGloss(0.3f);
    boxMaterial->setMetalness(0.0f);
    engine->root()->addChild(createPrimitive(engine.get(), "box", boxMaterial.get(),
        Vector3(-6.0f, 0.5f, -4.0f), Vector3(2.0f, 2.0f, 2.0f)));
    engine->root()->addChild(createPrimitive(engine.get(), "box", boxMaterial.get(),
        Vector3(6.0f, 1.0f, -6.0f), Vector3(3.0f, 3.0f, 3.0f)));

    auto sphereMaterial = std::make_shared<StandardMaterial>();
    sphereMaterial->setDiffuse(Color(0.85f, 0.85f, 0.9f));
    sphereMaterial->setGloss(0.6f);
    sphereMaterial->setMetalness(0.1f);
    engine->root()->addChild(createPrimitive(engine.get(), "sphere", sphereMaterial.get(),
        Vector3(-5.0f, 1.5f, 5.0f), Vector3(3.0f, 3.0f, 3.0f)));

    const auto statueResource = statue->resource();
    if (statueResource) {
        auto* statueEntity = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
        statueEntity->setLocalPosition(0.0f, -0.5f, 0.0f);
        engine->root()->addChild(statueEntity);
    } else {
        spdlog::warn("Failed to load statue model; continuing with primitives only");
    }

    // --- Orbit camera ---
    const auto start = Vector3(0.0f, 12.0f, 26.0f);
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();
    camera->setPosition(start);
    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(Vector3(0.0f, 3.0f, 0.0f));
    cameraControls->setEnableFly(false);
    cameraControls->setOrbitDistance(30.0f);
    cameraControls->storeResetState();

    bool animateSun = true;
    spdlog::info("Procedural sky (Nishita atmosphere) example.");
    spdlog::info("The single directional sun drives both the sky scattering and the ground lighting.");
    spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel zoom, R reset. SPACE = pause/resume sun.");

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float dayTime = 0.0f; // 0..1 across a sunrise->noon->sunset sweep

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                animateSun = !animateSun;
                spdlog::info("Sun animation: {}", animateSun ? "RUNNING" : "PAUSED");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            } else if (event.type == SDL_EVENT_PINCH_UPDATE && cameraControls) {
                cameraControls->addZoomInput((event.pinch.scale - 1.0f) * 10.0f);
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (animateSun) {
            // ~30 s for a full sunrise -> sunset pass.
            dayTime += static_cast<float>(dtSeconds) / 30.0f;
            if (dayTime > 1.0f) {
                dayTime -= 1.0f;
            }
            // Elevation: rises from ~4 deg, peaks near ~78 deg at midday, sets again.
            const float elevation = 4.0f + 74.0f * std::sin(dayTime * static_cast<float>(M_PI));
            // Azimuth: sweeps east (-90) to west (+90) across the day.
            const float azimuth = -90.0f + 180.0f * dayTime;
            applySun(elevation, azimuth);
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();

    spdlog::info("*** Visualization Engine Finished *** ");
    return 0;
}
