// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Post-processing compose-chain showcase (port of upstream graphics/post-processing).
//
// A statue hero on a floor, ringed by bright EMISSIVE orbiting orbs so BLOOM is
// obvious, all viewed through the full camera compose chain:
//   ACES tonemapping -> bloom -> vignette -> color grading (contrast/saturation)
//   -> color enhance (vibrance) -> fringing (chromatic aberration) -> a subtle DOF.
//
// Every effect is individually toggleable at runtime so the contribution of each
// stage is visible in isolation. Keys are logged at startup and on each change.
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

// Statue hero — the DOF focal subject.
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

    spdlog::info("*** VisuTwin Post-Processing Example Started ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Post-Processing Example", WINDOW_WIDTH, WINDOW_HEIGHT,
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
    scene->setExposure(1.6f);
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.15f, 0.15f, 0.18f);

    // Shadow-casting directional key light.
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComp = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComp->setColor(Color(1.0f, 0.95f, 0.85f, 1.0f));
        lightComp->setIntensity(1.4f);
        lightComp->setCastShadows(true);
        lightComp->setShadowResolution(2048);
        lightComp->setShadowDistance(60.0f);
        lightComp->setShadowBias(0.2f);
        lightComp->setShadowNormalBias(0.05f);
    }
    light->setLocalEulerAngles(45, 30, 0);
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

    // Floor.
    auto floorMat = std::make_shared<StandardMaterial>();
    floorMat->setDiffuse(Color(0.12f, 0.12f, 0.14f, 1.0f));
    floorMat->setMetalness(0.0f);
    floorMat->setGloss(0.55f);
    materials.push_back(floorMat);
    engine->root()->addChild(createEntity(
        engine.get(), floorMat.get(), "plane", Vector3(0.0f, 0.0f, 0.0f), Vector3(40.0f, 1.0f, 40.0f)
    ));

    // Statue hero (DOF focal subject).
    Entity* statueEntity = nullptr;
    const auto statueResource = statue->resource();
    if (statueResource) {
        statueEntity = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
        statueEntity->setLocalScale(0.5f, 0.5f, 0.5f);
        statueEntity->setLocalPosition(0.0f, 0.0f, 0.0f);
        engine->root()->addChild(statueEntity);
    } else {
        spdlog::warn("Failed to load statue model — using a placeholder box hero");
        auto heroMat = std::make_shared<StandardMaterial>();
        heroMat->setDiffuse(Color(0.8f, 0.8f, 0.82f, 1.0f));
        heroMat->setMetalness(0.1f);
        heroMat->setGloss(0.6f);
        materials.push_back(heroMat);
        statueEntity = createEntity(engine.get(), heroMat.get(), "box",
            Vector3(0.0f, 3.0f, 0.0f), Vector3(3.0f, 6.0f, 3.0f));
        engine->root()->addChild(statueEntity);
    }

    // Bright EMISSIVE orbiting orbs — the bloom sources.
    struct Orb {
        Entity* entity = nullptr;
        float radius = 0.0f;
        float speed = 0.0f;
        float phase = 0.0f;
        float height = 0.0f;
    };
    std::vector<Orb> orbs;
    const Color emissiveColors[] = {
        Color(1.0f, 0.15f, 0.1f, 1.0f),   // red
        Color(0.15f, 1.0f, 0.25f, 1.0f),  // green
        Color(0.2f, 0.35f, 1.0f, 1.0f),   // blue
        Color(1.0f, 0.75f, 0.1f, 1.0f),   // amber
        Color(0.9f, 0.15f, 1.0f, 1.0f),   // magenta
        Color(0.1f, 0.95f, 1.0f, 1.0f),   // cyan
    };
    const int orbCount = 6;
    for (int i = 0; i < orbCount; ++i) {
        auto mat = std::make_shared<StandardMaterial>();
        mat->setDiffuse(Color(0.02f, 0.02f, 0.02f, 1.0f));
        mat->setMetalness(0.0f);
        mat->setGloss(0.2f);
        mat->setEmissive(emissiveColors[i]);
        mat->setEmissiveIntensity(6.0f); // > 1 pushes into bloom range
        materials.push_back(mat);

        Orb orb;
        orb.radius = 9.0f;
        orb.speed = 0.35f + 0.05f * static_cast<float>(i);
        orb.phase = static_cast<float>(i) * (2.0f * 3.14159265f / orbCount);
        orb.height = 3.0f + 1.5f * std::sin(static_cast<float>(i));
        orb.entity = createEntity(engine.get(), mat.get(), "sphere",
            Vector3(orb.radius, orb.height, 0.0f), Vector3(0.9f, 0.9f, 0.9f));
        engine->root()->addChild(orb.entity);
        orbs.push_back(orb);
    }

    const Vector3 focusPoint(0.0f, 3.0f, 0.0f);
    const float sceneRadius = 12.0f;
    const float orbitDist = 26.0f;

    // Camera.
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();

    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setNearClip(0.5f);
        cameraComp->camera()->setFarClip(200.0f);
        cameraComp->camera()->setFov(55.0f);
    }

    // ── Configure the full compose chain ──────────────────────────────────
    if (cameraComp) {
        // TAA — steadies bloom/fringing under motion (subtle, on by default).
        auto taa = cameraComp->taa();
        taa.enabled = true;
        taa.highQuality = true;
        taa.jitter = 1.0f;
        cameraComp->setTaa(taa);

        auto rendering = cameraComp->rendering();
        rendering.toneMapping = TONEMAP_ACES;
        rendering.sharpness = 0.4f;

        // Bloom — makes the emissive orbs glow.
        rendering.bloomIntensity = 0.04f;

        // Vignette — darken the frame edges.
        rendering.vignetteEnabled = true;
        rendering.vignetteInner = 0.35f;
        rendering.vignetteOuter = 1.1f;
        rendering.vignetteCurvature = 0.5f;
        rendering.vignetteIntensity = 0.55f;

        // Color grading — a touch more contrast + saturation, cool tint.
        rendering.gradingEnabled = true;
        rendering.gradingBrightness = 1.0f;
        rendering.gradingContrast = 1.08f;
        rendering.gradingSaturation = 1.15f;
        rendering.gradingTint[0] = 0.98f;
        rendering.gradingTint[1] = 1.0f;
        rendering.gradingTint[2] = 1.05f;

        // Color enhance — lift vibrance a little.
        rendering.colorEnhanceVibrance = 0.25f;

        // Fringing — subtle chromatic aberration.
        rendering.fringingIntensity = 12.0f;

        cameraComp->setRendering(rendering);

        // Depth of field — focus the statue, blur the orbiting orbs.
        auto dof = cameraComp->dof();
        dof.enabled = true;
        dof.nearBlur = true;
        dof.focusDistance = orbitDist;
        dof.focusRange = 8.0f;
        dof.blurRadius = 3.0f;
        dof.blurRings = 4;
        dof.blurRingPoints = 5;
        dof.highQuality = true;
        cameraComp->setDof(dof);
    }

    camera->setPosition(focusPoint + Vector3(0.0f, 4.0f, orbitDist));
    engine->root()->addChild(camera);

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
    auto logState = [&](const char* reason) {
        if (!cameraComp) { return; }
        const auto& r = cameraComp->rendering();
        const auto& d = cameraComp->dof();
        spdlog::info(
            "[{}] tonemap={} bloom={:.3f} vignette={} grading={} vibrance={:.2f} fringing={:.1f} dof={} sharpness={:.2f}",
            reason,
            r.toneMapping == TONEMAP_ACES ? "ACES" : (r.toneMapping == TONEMAP_NONE ? "NONE" : "LINEAR"),
            r.bloomIntensity,
            r.vignetteEnabled ? "ON" : "OFF",
            r.gradingEnabled ? "ON" : "OFF",
            r.colorEnhanceVibrance,
            r.fringingIntensity,
            d.enabled ? "ON" : "OFF",
            r.sharpness);
    };

    spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
    spdlog::info("Compose toggles:");
    spdlog::info("  B  bloom on/off");
    spdlog::info("  V  vignette on/off");
    spdlog::info("  G  color grading on/off");
    spdlog::info("  C  color enhance (vibrance) on/off");
    spdlog::info("  X  fringing (chromatic aberration) on/off");
    spdlog::info("  D  depth of field on/off");
    spdlog::info("  T  TAA on/off");
    spdlog::info("  M  cycle tonemapping (ACES -> NONE -> LINEAR)");
    spdlog::info("  S  cycle sharpness (0 -> 0.25 -> 0.5 -> 0.75 -> 1.0)");
    spdlog::info("  0  disable ALL compose effects   1  restore ALL");
    spdlog::info("  ESC quit");
    logState("init");

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float totalTime = 0.0f;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;

            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_B && cameraComp) {
                auto r = cameraComp->rendering();
                r.bloomIntensity = r.bloomIntensity > 0.0f ? 0.0f : 0.04f;
                cameraComp->setRendering(r);
                logState("bloom");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_V && cameraComp) {
                auto r = cameraComp->rendering();
                r.vignetteEnabled = !r.vignetteEnabled;
                cameraComp->setRendering(r);
                logState("vignette");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_G && cameraComp) {
                auto r = cameraComp->rendering();
                r.gradingEnabled = !r.gradingEnabled;
                cameraComp->setRendering(r);
                logState("grading");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_C && cameraComp) {
                auto r = cameraComp->rendering();
                r.colorEnhanceVibrance = r.colorEnhanceVibrance > 0.0f ? 0.0f : 0.25f;
                cameraComp->setRendering(r);
                logState("color-enhance");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_X && cameraComp) {
                auto r = cameraComp->rendering();
                r.fringingIntensity = r.fringingIntensity > 0.0f ? 0.0f : 12.0f;
                cameraComp->setRendering(r);
                logState("fringing");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_D && cameraComp) {
                auto d = cameraComp->dof();
                d.enabled = !d.enabled;
                cameraComp->setDof(d);
                logState("dof");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_T && cameraComp) {
                auto taa = cameraComp->taa();
                taa.enabled = !taa.enabled;
                cameraComp->setTaa(taa);
                spdlog::info("[taa] {}", taa.enabled ? "ON" : "OFF");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_M && cameraComp) {
                auto r = cameraComp->rendering();
                if (r.toneMapping == TONEMAP_ACES) r.toneMapping = TONEMAP_NONE;
                else if (r.toneMapping == TONEMAP_NONE) r.toneMapping = TONEMAP_LINEAR;
                else r.toneMapping = TONEMAP_ACES;
                cameraComp->setRendering(r);
                scene->setToneMapping(r.toneMapping);
                logState("tonemap");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_S && cameraComp) {
                auto r = cameraComp->rendering();
                if (r.sharpness < 0.125f) r.sharpness = 0.25f;
                else if (r.sharpness < 0.375f) r.sharpness = 0.5f;
                else if (r.sharpness < 0.625f) r.sharpness = 0.75f;
                else if (r.sharpness < 0.875f) r.sharpness = 1.0f;
                else r.sharpness = 0.0f;
                cameraComp->setRendering(r);
                logState("sharpness");

            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_0 && cameraComp) {
                auto r = cameraComp->rendering();
                r.bloomIntensity = 0.0f;
                r.vignetteEnabled = false;
                r.gradingEnabled = false;
                r.colorEnhanceVibrance = 0.0f;
                r.fringingIntensity = 0.0f;
                r.sharpness = 0.0f;
                cameraComp->setRendering(r);
                auto d = cameraComp->dof();
                d.enabled = false;
                cameraComp->setDof(d);
                logState("all-off");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_1 && cameraComp) {
                auto r = cameraComp->rendering();
                r.bloomIntensity = 0.04f;
                r.vignetteEnabled = true;
                r.gradingEnabled = true;
                r.colorEnhanceVibrance = 0.25f;
                r.fringingIntensity = 12.0f;
                r.sharpness = 0.4f;
                cameraComp->setRendering(r);
                auto d = cameraComp->dof();
                d.enabled = true;
                cameraComp->setDof(d);
                logState("all-on");

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
        totalTime += dt;

        // Orbit the emissive orbs around the hero.
        for (const auto& orb : orbs) {
            if (!orb.entity) { continue; }
            const float a = orb.phase + totalTime * orb.speed * 2.0f * 3.14159265f;
            orb.entity->setLocalPosition(
                orb.radius * std::cos(a),
                orb.height,
                orb.radius * std::sin(a));
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** VisuTwin Post-Processing Example Finished ***");

    return 0;
}
