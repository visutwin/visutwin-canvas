// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Clustered lighting example — mirrors PlayCanvas graphics/clustered-lighting.
// Many colored local lights (omni + spot) light a field of objects, plus a few
// objects. With Scene::setClusteredLightingEnabled(true) the renderer buckets the
// shadow-casting spots. The unshadowed lights are bucketed into a 3D world-space grid
// (WorldClusters); shadow-casting spots render into a per-light depth-array shadow atlas.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/constants.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1100;
constexpr int WINDOW_HEIGHT = 750;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

// A local light + a small emissive marker sphere/cone so its position is visible.
struct DemoLight
{
    Entity* light = nullptr;
    Entity* marker = nullptr;
    float phase = 0.0f;
    float radius = 0.0f;
    float height = 0.0f;
    bool spot = false;
};

Entity* createPrimitive(Engine* engine, const char* type, StandardMaterial* material,
                        const Vector3& pos, const Vector3& scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(pos.getX(), pos.getY(), pos.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
    if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
        render->setMaterial(material);
        render->setType(type);
        render->setCastShadows(true);
        render->setReceiveShadows(true);
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

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Clustered Lighting", WINDOW_WIDTH, WINDOW_HEIGHT,
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
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    // Enable clustered lighting: unshadowed local lights are bucketed into a 3D grid so many
    // can be evaluated cheaply; shadow-casting local lights still cast real shadows.
    scene->setClusteredLightingEnabled(true);
    scene->setToneMapping(TONEMAP_ACES);
    scene->setExposure(1.0f);
    scene->setAmbientLight(0.06f, 0.06f, 0.08f);

    // --- Ground plane ---
    auto* groundMaterial = new StandardMaterial();
    groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.55f));
    groundMaterial->setMetalness(0.0f);
    groundMaterial->setGloss(0.35f);
    auto* ground = createPrimitive(engine.get(), "plane", groundMaterial,
        Vector3(0.0f, 0.0f, 0.0f), Vector3(120.0f, 1.0f, 120.0f));
    engine->root()->addChild(ground);

    // --- A field of receiver objects (boxes + spheres) to catch the light ---
    auto* objMaterial = new StandardMaterial();
    objMaterial->setDiffuse(Color(0.8f, 0.8f, 0.8f));
    objMaterial->setMetalness(0.0f);
    objMaterial->setGloss(0.4f);
    for (int gx = -4; gx <= 4; ++gx) {
        for (int gz = -4; gz <= 4; ++gz) {
            const float x = static_cast<float>(gx) * 9.0f;
            const float z = static_cast<float>(gz) * 9.0f;
            const bool box = ((gx + gz) & 1) == 0;
            engine->root()->addChild(createPrimitive(engine.get(),
                box ? "box" : "sphere", objMaterial,
                Vector3(x, 1.5f, z), Vector3(2.4f, box ? 3.0f : 2.4f, 2.4f)));
        }
    }

    // --- Many colored local lights (no shadows) with emissive markers ---
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> col(0.2f, 1.0f);
    std::uniform_real_distribution<float> ang(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> rad(12.0f, 46.0f);
    std::uniform_real_distribution<float> omniH(3.0f, 11.0f);
    std::uniform_real_distribution<float> spotH(14.0f, 22.0f);

    std::vector<DemoLight> lights;
    auto addLight = [&](bool spot) {
        DemoLight dl;
        dl.spot = spot;
        dl.phase = ang(rng);
        dl.radius = rad(rng);
        // Vary heights so orbiting lights don't all pile into the same grid cell
        // (each cluster cell holds a bounded number of lights).
        dl.height = spot ? spotH(rng) : omniH(rng);

        const Color color(col(rng), col(rng), col(rng), 1.0f);

        auto* light = new Entity();
        light->setEngine(engine.get());
        if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lc->setType(spot ? LightType::LIGHTTYPE_SPOT : LightType::LIGHTTYPE_OMNI);
            lc->setColor(color);
            lc->setIntensity(2.0f);
            lc->setRange(spot ? 30.0f : 14.0f);
            lc->setCastShadows(false);
            if (spot) {
                lc->setInnerConeAngle(20.0f);
                lc->setOuterConeAngle(35.0f);
            }
        }
        if (spot) {
            light->setLocalEulerAngles(90.0f, 0.0f, 0.0f); // point downward
        }
        engine->root()->addChild(light);
        dl.light = light;

        // Emissive marker so the light position is visible.
        auto* markerMat = new StandardMaterial();
        markerMat->setDiffuse(Color(0.0f, 0.0f, 0.0f));
        markerMat->setEmissive(color);
        markerMat->setEmissiveIntensity(4.0f);
        dl.marker = createPrimitive(engine.get(), spot ? "cone" : "sphere", markerMat,
            Vector3(0.0f, dl.height, 0.0f), Vector3(0.7f, 0.7f, 0.7f));
        engine->root()->addChild(dl.marker);

        lights.push_back(dl);
    };

    for (int i = 0; i < 16; ++i) addLight(false); // omni
    for (int i = 0; i < 4; ++i)  addLight(true);  // spot

    // --- Many SHADOW-CASTING spot lights ---
    // Each renders into its own slice of the clustered shadow atlas (a depth
    // texture2d_array) and is sampled directly by the clustered fragment shader —
    // so the count is NOT bounded by the 8-slot main light array. Here we place 12
    // (> 8) angled shadow-casting spots so their objects drop shadows on the ground.
    auto addShadowSpot = [&](const Vector3& pos, float pitchDeg, float yawDeg, const Color& color) {
        auto* light = new Entity();
        light->setEngine(engine.get());
        if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lc->setType(LightType::LIGHTTYPE_SPOT);
            lc->setColor(color);
            lc->setIntensity(8.0f);
            lc->setRange(90.0f);
            lc->setInnerConeAngle(26.0f);
            lc->setOuterConeAngle(42.0f);
            lc->setCastShadows(true);
            lc->setShadowResolution(1024);
        }
        light->setLocalPosition(pos.getX(), pos.getY(), pos.getZ());
        // Tilted off vertical (90° = straight down) so each object drops a shadow
        // that stretches across the ground instead of hiding directly beneath it.
        light->setLocalEulerAngles(pitchDeg, yawDeg, 0.0f);
        engine->root()->addChild(light);

        auto* markerMat = new StandardMaterial();
        markerMat->setDiffuse(Color(0.0f, 0.0f, 0.0f));
        markerMat->setEmissive(color);
        markerMat->setEmissiveIntensity(5.0f);
        engine->root()->addChild(createPrimitive(engine.get(), "cone", markerMat,
            pos, Vector3(1.2f, 1.2f, 1.2f)));
    };
    {
        const Color spotCols[4] = {
            Color(1.0f, 1.0f, 1.0f), Color(1.0f, 0.85f, 0.6f),
            Color(0.6f, 0.85f, 1.0f), Color(0.8f, 1.0f, 0.7f)
        };
        int si = 0;
        for (int cx = 0; cx < 4; ++cx) {
            for (int cz = 0; cz < 3; ++cz) {
                const float x = -27.0f + static_cast<float>(cx) * 18.0f;
                const float z = -18.0f + static_cast<float>(cz) * 18.0f;
                // All angled the same way so shadows stretch consistently toward the camera.
                addShadowSpot(Vector3(x, 30.0f, z + 6.0f), 58.0f, 0.0f, spotCols[si++ % 4]);
            }
        }
    }

    // --- A single dim directional fill so shadowed areas aren't pure black ---
    auto* dirLight = new Entity();
    dirLight->setEngine(engine.get());
    if (auto* lc = static_cast<LightComponent*>(dirLight->addComponent<LightComponent>())) {
        lc->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        lc->setColor(Color(1.0f, 1.0f, 1.0f));
        lc->setIntensity(0.2f);
        lc->setCastShadows(false);
    }
    dirLight->setLocalEulerAngles(60.0f, 30.0f, 0.0f);
    engine->root()->addChild(dirLight);

    // --- Camera ---
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();
    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setClearColor(Color(0.02f, 0.02f, 0.03f, 1.0f));
        cameraComp->camera()->setFarClip(400.0f);
    }
    camera->setPosition(Vector3(0.0f, 45.0f, 70.0f));
    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(Vector3(0.0f, 2.0f, 0.0f));
    cameraControls->setEnableFly(false);
    cameraControls->setMoveSpeed(40.0f);
    cameraControls->setOrbitDistance(85.0f);
    cameraControls->storeResetState();

    spdlog::info("*** VisuTwin Clustered Lighting Example ***");
    spdlog::info("{} clustered local lights + 12 atlas shadow-casting spots.", lights.size());
    spdlog::info("Orbit: LMB/RMB, Wheel zoom, R reset, Esc quit.");

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
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(static_cast<double>(nowCounter - prevCounter) /
                                            static_cast<double>(perfFreq));
        prevCounter = nowCounter;
        time += dt;

        // Orbit the lights around the field so the clustering updates every frame.
        for (auto& dl : lights) {
            const float a = dl.phase + time * (dl.spot ? 0.25f : 0.4f);
            const float x = std::cos(a) * dl.radius;
            const float z = std::sin(a) * dl.radius;
            dl.light->setLocalPosition(x, dl.height, z);
            if (dl.marker) {
                dl.marker->setLocalPosition(x, dl.height, z);
            }
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();
    spdlog::info("*** VisuTwin Clustered Lighting Example Finished ***");
    return 0;
}
