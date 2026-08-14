// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Depth-of-field showcase — port of upstream graphics/depth-of-field.
//
// An apartment interior lit purely by the helipad environment atlas, with an
// Egyptian cat statue on the floor at the focus plane and an emissive neon "love"
// sign blooming on the far wall. Bokeh DOF keeps the cat razor-sharp while the
// near furniture and the far end of the room dissolve into blur.
//
// Upstream drives DOF from a control panel; this port maps the same settings onto
// keys (listed at startup), all going through CameraComponent::dof().
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
#include <string>
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

// Environment atlas — the scene's only light source (IBL + skydome).
const auto envAtlas = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

const auto apartment = std::make_unique<Asset>(
    "apartment", AssetType::CONTAINER, rootPath + "/models/apartment.glb");

const auto love = std::make_unique<Asset>(
    "love", AssetType::CONTAINER, rootPath + "/models/love.glb");

const auto cat = std::make_unique<Asset>(
    "cat", AssetType::CONTAINER, rootPath + "/models/cat.glb");

// World-space AABB of every mesh instance under `entity`.
BoundingBox entityAabb(Entity* entity)
{
    BoundingBox bbox;
    bbox.setCenter(0.0f, 0.0f, 0.0f);
    bbox.setHalfExtents(0.0f, 0.0f, 0.0f);
    if (!entity) {
        return bbox;
    }

    bool any = false;
    for (auto* render : entity->findComponents<RenderComponent>()) {
        for (auto* meshInstance : render->meshInstances()) {
            if (!meshInstance) {
                continue;
            }
            if (!any) {
                bbox = meshInstance->aabb();
                any = true;
            } else {
                bbox.add(meshInstance->aabb());
            }
        }
    }

    if (!any) {
        bbox.setCenter(entity->position());
        bbox.setHalfExtents(0.5f, 0.5f, 0.5f);
    }
    return bbox;
}

// Runs `fn` for every material under `entity`.
template <typename Fn>
void forEachMaterial(Entity* entity, Fn&& fn)
{
    if (!entity) {
        return;
    }
    for (auto* render : entity->findComponents<RenderComponent>()) {
        for (auto* meshInstance : render->meshInstances()) {
            if (auto* material = meshInstance ? meshInstance->material() : nullptr) {
                fn(material);
            }
        }
    }
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

    // Skydome + IBL from the env atlas — there is no other light in the scene.
    const auto envAtlasResource = envAtlas->resource();
    if (!envAtlasResource) {
        spdlog::error("Failed to load the environment atlas — the scene has no other light");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
    scene->setExposure(1.2f);

    // ── Apartment interior ──────────────────────────────────────────────────
    const auto apartmentResource = apartment->resource();
    if (!apartmentResource) {
        spdlog::error("Failed to load models/apartment.glb");
        shutdown();
        return -1;
    }
    auto* apartmentEntity = std::get<ContainerResource*>(*apartmentResource)->instantiateRenderEntity();
    apartmentEntity->setLocalScale(30.0f, 30.0f, 30.0f);
    engine->root()->addChild(apartmentEntity);

    // ── Neon "love" sign on the far wall ────────────────────────────────────
    const auto loveResource = love->resource();
    if (!loveResource) {
        spdlog::error("Failed to load models/love.glb");
        shutdown();
        return -1;
    }
    auto* loveEntity = std::get<ContainerResource*>(*loveResource)->instantiateRenderEntity();
    loveEntity->setLocalPosition(-335.0f, 180.0f, 0.0f);
    loveEntity->setLocalScale(130.0f, 130.0f, 130.0f);
    engine->root()->addChild(loveEntity);

    // Make the neon tube emissive enough to bloom.
    if (auto* neon = dynamic_cast<Entity*>(loveEntity->findByName("s.0009_Standard_FF00BB_0"))) {
        forEachMaterial(neon, [](Material* material) {
            if (auto* standard = dynamic_cast<StandardMaterial*>(material)) {
                standard->setEmissiveIntensity(200.0f);
            }
        });
    } else {
        spdlog::warn("Neon mesh 's.0009_Standard_FF00BB_0' not found — the sign will not bloom");
    }

    // The sign's glass uses transmission; dynamic refraction is not wanted here.
    forEachMaterial(loveEntity, [](Material* material) {
        if (auto* standard = dynamic_cast<StandardMaterial*>(material)) {
            standard->setUseDynamicRefraction(false);
        }
    });

    // ── Cat statue: the focal object ────────────────────────────────────────
    const auto catResource = cat->resource();
    if (!catResource) {
        spdlog::error("Failed to load models/cat.glb");
        shutdown();
        return -1;
    }
    auto* catEntity = std::get<ContainerResource*>(*catResource)->instantiateRenderEntity();
    catEntity->setLocalPosition(-80.0f, 80.0f, -20.0f);
    catEntity->setLocalScale(80.0f, 80.0f, 80.0f);
    engine->root()->addChild(catEntity);

    // ── Camera ──────────────────────────────────────────────────────────────
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();

    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setNearClip(0.1f);
        cameraComp->camera()->setFarClip(1500.0f);
        cameraComp->camera()->setFov(80.0f);
    }

    if (cameraComp) {
        // Bokeh depth of field, upstream's defaults.
        auto dof = cameraComp->dof();
        dof.enabled = true;
        dof.nearBlur = true;               // blur nearer-than-focus too
        dof.focusDistance = 200.0f;        // sharp plane distance from camera
        dof.focusRange = 100.0f;           // depth window kept in focus
        dof.blurRadius = 5.0f;             // bokeh circle-of-confusion size
        dof.blurRings = 4;                 // concentric bokeh sample rings
        dof.blurRingPoints = 5;            // samples per ring
        dof.highQuality = true;
        cameraComp->setDof(dof);

        auto rendering = cameraComp->rendering();
        rendering.toneMapping = TONEMAP_ACES;
        rendering.samples = 4;             // 4x MSAA on the offscreen scene target
        rendering.bloomIntensity = 0.03f;
        rendering.bloomBlurLevel = 7;      // tighter glow than the 16-level default
        rendering.vignetteEnabled = true;
        rendering.vignetteInner = 0.5f;
        rendering.vignetteOuter = 1.0f;
        rendering.vignetteCurvature = 0.5f;
        rendering.vignetteIntensity = 0.5f;
        cameraComp->setRendering(rendering);
    }

    camera->setLocalPosition(-50.0f, 100.0f, 220.0f);
    engine->root()->addChild(camera);

    // Upstream's orbit camera aims at the focus entity's AABB centre on initialize
    // (its own lookAt(0, 0, 100) never survives), keeping the camera position and
    // deriving the orbit distance from it — which is exactly what setFocusPoint does.
    const BoundingBox catBbox = entityAabb(catEntity);
    const Vector3 focusPoint = catBbox.center();
    const float sceneRadius = std::max(catBbox.halfExtents().length(), 1.0f);

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(focusPoint);
    cameraControls->setEnableFly(false);
    cameraControls->setMoveSpeed(2 * sceneRadius);
    cameraControls->setMoveFastSpeed(4 * sceneRadius);
    cameraControls->setMoveSlowSpeed(sceneRadius);
    cameraControls->storeResetState();
    const float orbitDistance = camera->position().distance(focusPoint);

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
                d.focusDistance = std::min(1400.0f, d.focusDistance + 10.0f);
                cameraComp->setDof(d);
                logDof("focus-farther");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_COMMA && cameraComp) {
                auto d = cameraComp->dof();
                d.focusRange = std::max(1.0f, d.focusRange - 5.0f);
                cameraComp->setDof(d);
                logDof("range-narrow");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_PERIOD && cameraComp) {
                auto d = cameraComp->dof();
                d.focusRange = std::min(500.0f, d.focusRange + 5.0f);
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
                cameraControls->focus(focusPoint, orbitDistance);
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
