// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Depth-of-field showcase (port of upstream graphics/depth-of-field).
//
// A long receding row of colored spheres marches away from the camera down a
// floor, so the bokeh depth-of-field blur is obvious: objects at the focus
// distance are razor-sharp while both nearer AND farther objects dissolve into
// blur (when near-blur is on). A statue hero sits at the focus plane. DOF is
// configured entirely through CameraComponent::dof().
//
// The focus distance is driven interactively so the sharp plane visibly sweeps
// up and down the row; DOF, near-blur and quality are all toggleable and every
// change is logged.
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
#include <vector>

#include <QuartzCore/QuartzCore.hpp>

#include <core/shape/boundingBox.h>
#include <framework/assets/asset.h>

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

constexpr int WINDOW_WIDTH = 1024;
constexpr int WINDOW_HEIGHT = 768;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Environment atlas for IBL / skybox.
const auto envAtlas = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

// Statue hero — sits on the focus plane at the head of the row.
const auto statue = std::make_unique<Asset>(
    "statue",
    AssetType::CONTAINER,
    rootPath + "/models/statue.glb"
);

Entity* createEntity(Engine* engine, Material* material, const char* type,
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

    spdlog::info("*** Depth-of-Field Example Started ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Depth-of-Field Example", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) { std::cerr << "SDL Window Creation Failed" << std::endl; shutdown(); return -1; }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { std::cerr << "SDL Renderer Creation Failed" << std::endl; shutdown(); return -1; }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) { std::cerr << "Unable to get render Metal layer" << std::endl; shutdown(); return -1; }

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) { std::cerr << "Unable to create graphics device" << std::endl; shutdown(); return -1; }

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

    // Skydome + IBL from the env atlas.
    scene->setSkyboxMip(1);
    scene->setExposure(1.4f);
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.15f, 0.15f, 0.18f);

    // Shadow-casting directional key light.
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComp = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComp->setColor(Color(1.0f, 0.96f, 0.88f, 1.0f));
        lightComp->setIntensity(1.5f);
        lightComp->setCastShadows(true);
        lightComp->setShadowResolution(2048);
        lightComp->setShadowDistance(220.0f);
        lightComp->setShadowBias(0.2f);
        lightComp->setShadowNormalBias(0.05f);
    }
    light->setLocalEulerAngles(50, 25, 0);
    engine->root()->addChild(light);

    // Env atlas (skybox + specular/diffuse IBL).
    const auto envAtlasResource = envAtlas->resource();
    if (envAtlasResource) {
        scene->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
    } else {
        spdlog::warn("Failed to load environment atlas — continuing without IBL");
    }

    // Materials kept alive for the lifetime of the program.
    std::vector<std::shared_ptr<StandardMaterial>> materials;

    // Long floor stretching down the row.
    auto floorMat = std::make_shared<StandardMaterial>();
    floorMat->setDiffuse(Color(0.10f, 0.10f, 0.12f, 1.0f));
    floorMat->setMetalness(0.0f);
    floorMat->setGloss(0.5f);
    materials.push_back(floorMat);
    engine->root()->addChild(createEntity(
        engine.get(), floorMat.get(), "plane", Vector3(0.0f, 0.0f, -100.0f), Vector3(40.0f, 1.0f, 260.0f)
    ));

    // ── A long receding row of spheres marching away from the camera ────────
    // The camera sits near z = +CAMERA_Z looking down -Z. Objects at increasing
    // depth reveal the DOF blur falloff on both sides of the focus plane.
    const Color rowColors[] = {
        Color(0.90f, 0.20f, 0.20f, 1.0f),
        Color(0.95f, 0.55f, 0.15f, 1.0f),
        Color(0.90f, 0.85f, 0.20f, 1.0f),
        Color(0.30f, 0.85f, 0.30f, 1.0f),
        Color(0.20f, 0.70f, 0.90f, 1.0f),
        Color(0.30f, 0.40f, 0.95f, 1.0f),
        Color(0.65f, 0.30f, 0.90f, 1.0f),
        Color(0.90f, 0.35f, 0.75f, 1.0f),
    };
    constexpr int rowCount = 11;
    constexpr float rowSpacing = 18.0f;   // world units between objects
    constexpr float rowStartZ = 0.0f;     // first object sits at z = 0
    for (int i = 0; i < rowCount; ++i) {
        auto mat = std::make_shared<StandardMaterial>();
        mat->setDiffuse(rowColors[i % (sizeof(rowColors) / sizeof(rowColors[0]))]);
        mat->setMetalness(0.1f);
        mat->setGloss(0.6f);
        materials.push_back(mat);

        const float z = rowStartZ - static_cast<float>(i) * rowSpacing;
        // Alternate spheres and boxes, staggered left/right so more are visible.
        const bool isBox = (i % 2) == 1;
        const float xOffset = (i % 2 == 0) ? -3.0f : 3.0f;
        engine->root()->addChild(createEntity(
            engine.get(), mat.get(), isBox ? "box" : "sphere",
            Vector3(xOffset, 4.0f, z), Vector3(6.0f, 6.0f, 6.0f)
        ));
    }

    // Statue hero placed mid-row, on the initial focus plane.
    const float statueZ = rowStartZ - 4.0f * rowSpacing;
    const auto statueResource = statue->resource();
    if (statueResource) {
        auto* statueEntity = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
        statueEntity->setLocalScale(0.6f, 0.6f, 0.6f);
        statueEntity->setLocalPosition(0.0f, 0.0f, statueZ);
        engine->root()->addChild(statueEntity);
    } else {
        spdlog::warn("Failed to load statue model — the row of objects alone shows the DOF effect");
    }

    // Camera looks straight down the row along -Z.
    constexpr float cameraZ = 30.0f;
    constexpr float cameraY = 8.0f;
    const Vector3 cameraPos(0.0f, cameraY, cameraZ);
    const Vector3 focusPoint(0.0f, 4.0f, statueZ);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();

    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setNearClip(0.5f);
        cameraComp->camera()->setFarClip(500.0f);
        cameraComp->camera()->setFov(55.0f);
    }

    // ── Depth of field: focus the statue, blur near and far objects ─────────
    // Initial focus distance = distance from the camera to the statue plane.
    float focusDistance = cameraZ - statueZ; // ~102 world units
    if (cameraComp) {
        auto dof = cameraComp->dof();
        dof.enabled = true;
        dof.nearBlur = true;               // blur nearer-than-focus too
        dof.focusDistance = focusDistance; // sharp plane distance from camera
        dof.focusRange = 20.0f;            // depth window kept in focus
        dof.blurRadius = 5.0f;             // bokeh circle-of-confusion size
        dof.blurRings = 4;                 // concentric bokeh sample rings
        dof.blurRingPoints = 5;            // samples per ring
        dof.highQuality = true;
        cameraComp->setDof(dof);

        // TAA steadies the bokeh under motion.
        auto taa = cameraComp->taa();
        taa.enabled = true;
        taa.highQuality = true;
        taa.jitter = 1.0f;
        cameraComp->setTaa(taa);

        auto rendering = cameraComp->rendering();
        rendering.toneMapping = TONEMAP_ACES;
        rendering.sharpness = 0.4f;
        cameraComp->setRendering(rendering);
    }

    camera->setPosition(cameraPos.getX(), cameraPos.getY(), cameraPos.getZ());
    engine->root()->addChild(camera);

    const float sceneRadius = 60.0f;
    const float orbitDist = cameraZ - statueZ;

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(focusPoint);
    cameraControls->setEnableFly(false);
    cameraControls->setAutoFarClip(true);
    cameraControls->setMoveSpeed(2 * sceneRadius);
    cameraControls->setMoveFastSpeed(4 * sceneRadius);
    cameraControls->setMoveSlowSpeed(sceneRadius);
    cameraControls->setOrbitDistance(orbitDist);
    cameraControls->storeResetState();

    // ── State logging ──────────────────────────────────────────────────────
    auto logDof = [&](const char* reason) {
        if (!cameraComp) { return; }
        const auto& d = cameraComp->dof();
        spdlog::info(
            "[DOF {}] enabled={} nearBlur={} focusDistance={:.1f} focusRange={:.1f} "
            "blurRadius={:.1f} rings={} ringPoints={} highQuality={}",
            reason,
            d.enabled ? "ON" : "OFF",
            d.nearBlur ? "ON" : "OFF",
            d.focusDistance, d.focusRange, d.blurRadius,
            d.blurRings, d.blurRingPoints,
            d.highQuality ? "ON" : "OFF");
    };

    spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
    spdlog::info("Depth-of-field controls:");
    spdlog::info("  D        toggle DOF on/off");
    spdlog::info("  [        move focus distance CLOSER (-10)");
    spdlog::info("  ]        move focus distance FARTHER (+10)");
    spdlog::info("  , / .    narrow / widen focus range (+/-5)");
    spdlog::info("  - / =    smaller / larger blur radius (+/-1)");
    spdlog::info("  N        toggle near-blur");
    spdlog::info("  Q        toggle high-quality bokeh");
    spdlog::info("  ESC      quit");
    logDof("init");

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;

            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_D && cameraComp) {
                auto d = cameraComp->dof();
                d.enabled = !d.enabled;
                cameraComp->setDof(d);
                logDof("toggle");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_LEFTBRACKET && cameraComp) {
                auto d = cameraComp->dof();
                d.focusDistance = std::max(1.0f, d.focusDistance - 10.0f);
                cameraComp->setDof(d);
                logDof("focus-closer");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_RIGHTBRACKET && cameraComp) {
                auto d = cameraComp->dof();
                d.focusDistance = std::min(480.0f, d.focusDistance + 10.0f);
                cameraComp->setDof(d);
                logDof("focus-farther");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_COMMA && cameraComp) {
                auto d = cameraComp->dof();
                d.focusRange = std::max(1.0f, d.focusRange - 5.0f);
                cameraComp->setDof(d);
                logDof("range-narrow");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_PERIOD && cameraComp) {
                auto d = cameraComp->dof();
                d.focusRange = std::min(200.0f, d.focusRange + 5.0f);
                cameraComp->setDof(d);
                logDof("range-widen");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_MINUS && cameraComp) {
                auto d = cameraComp->dof();
                d.blurRadius = std::max(1.0f, d.blurRadius - 1.0f);
                cameraComp->setDof(d);
                logDof("blur-smaller");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_EQUALS && cameraComp) {
                auto d = cameraComp->dof();
                d.blurRadius = std::min(20.0f, d.blurRadius + 1.0f);
                cameraComp->setDof(d);
                logDof("blur-larger");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_N && cameraComp) {
                auto d = cameraComp->dof();
                d.nearBlur = !d.nearBlur;
                cameraComp->setDof(d);
                logDof("near-blur");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_Q && cameraComp) {
                auto d = cameraComp->dof();
                d.highQuality = !d.highQuality;
                cameraComp->setDof(d);
                logDof("high-quality");

            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(focusPoint, orbitDist);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            } else if (event.type == SDL_EVENT_PINCH_UPDATE && cameraControls) {
                const float pinchDelta = (event.pinch.scale - 1.0f) * 10.0f;
                cameraControls->addZoomInput(pinchDelta);
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;
        const float dt = static_cast<float>(dtSeconds);

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** Depth-of-Field Example Finished ***");

    return 0;
}
