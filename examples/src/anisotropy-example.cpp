// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Anisotropic specular demo (mirrors PlayCanvas materials/material-anisotropic).
//
// A grid of metallic procedural spheres, all lit by the helipad env atlas plus a
// single directional light:
//   * X axis (columns): anisotropy swept from -1 on the left to +1 on the right.
//     At 0 the highlight is a round GGX blob; as |anisotropy| grows the highlight
//     stretches into a brushed-metal streak, and the streak's orientation FLIPS
//     between negative and positive (tangent- vs bitangent-aligned).
//   * Z axis (rows): gloss stepped 0.6 -> 0.9 (fairly smooth metal — anisotropy is
//     only visible on a smooth metallic surface).
//
// DEVIATION: the engine's StandardMaterial exposes only setAnisotropy(float) — there
// is NO anisotropy rotation/direction setter (upstream's anisotropyRotation), so this
// example varies the single anisotropy scalar and cannot rotate the streak.
// Orbit camera (CameraControls) frames the grid.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/constants.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

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

// Grid dimensions: columns sweep anisotropy, rows sweep gloss.
constexpr int NUM_SPHERES_X = 9;   // anisotropy -1 .. +1
constexpr int NUM_SPHERES_Z = 4;   // gloss 0.6 .. 0.9
constexpr float SPACING = 1.5f;

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
        "Anisotropic Specular", WINDOW_WIDTH, WINDOW_HEIGHT,
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
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.15f, 0.15f, 0.18f);
    scene->setExposure(1.0f);
    scene->setSkyboxMip(1);
    scene->setSkyboxIntensity(0.6f);

    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad env atlas");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    // Single directional light (as upstream: base euler +90 X, -75 Y).
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lightComponent->setIntensity(1.5f);
    }
    light->setLocalEulerAngles(90.0f, -75.0f, 0.0f);
    engine->root()->addChild(light);

    // Keep material lifetimes alive for the app duration.
    std::vector<std::shared_ptr<StandardMaterial>> materials;
    materials.reserve(NUM_SPHERES_X * NUM_SPHERES_Z);

    for (int iz = 0; iz < NUM_SPHERES_Z; ++iz) {
        // gloss 0.6 .. 0.9 (smooth metal so the anisotropic streak is visible).
        const float gloss = 0.6f + 0.3f * (NUM_SPHERES_Z > 1
            ? static_cast<float>(iz) / static_cast<float>(NUM_SPHERES_Z - 1)
            : 0.0f);

        for (int ix = 0; ix < NUM_SPHERES_X; ++ix) {
            // anisotropy -1 .. +1 across the row (highlight stretch direction flips).
            const float aniso = -1.0f + 2.0f * (NUM_SPHERES_X > 1
                ? static_cast<float>(ix) / static_cast<float>(NUM_SPHERES_X - 1)
                : 0.0f);

            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(0.85f, 0.85f, 0.88f, 1.0f));
            material->setUseMetalness(true);
            material->setMetalness(1.0f);
            material->setGloss(gloss);
            material->setAnisotropy(aniso);
            materials.push_back(material);

            auto* sphere = new Entity();
            sphere->setEngine(engine.get());
            sphere->setLocalPosition(
                (static_cast<float>(ix) - (NUM_SPHERES_X - 1) * 0.5f) * SPACING,
                0.0f,
                (static_cast<float>(iz) - (NUM_SPHERES_Z - 1) * 0.5f) * SPACING
            );
            sphere->setLocalScale(0.7f, 0.7f, 0.7f);
            if (auto* render = static_cast<RenderComponent*>(sphere->addComponent<RenderComponent>())) {
                render->setMaterial(material.get());
                render->setType("sphere");
            }
            engine->root()->addChild(sphere);
        }
    }

    // Orbit camera framing the grid center.
    const Vector3 focusPoint(0.0f, 0.0f, 0.0f);
    const float gridRadius = std::max(NUM_SPHERES_X, NUM_SPHERES_Z) * SPACING * 0.5f;

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();
    camera->setPosition(Vector3(0.0f, gridRadius * 0.9f, gridRadius * 1.8f));
    engine->root()->addChild(camera);
    (void)cameraComponent;

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(focusPoint);
    cameraControls->setEnableFly(false);
    cameraControls->setMoveSpeed(2.0f * gridRadius);
    cameraControls->setMoveFastSpeed(4.0f * gridRadius);
    cameraControls->setMoveSlowSpeed(gridRadius);
    cameraControls->setOrbitDistance(std::max(gridRadius * 2.2f, 10.0f));
    cameraControls->storeResetState();

    spdlog::info("Anisotropic specular: {}x{} metallic sphere grid.", NUM_SPHERES_X, NUM_SPHERES_Z);
    spdlog::info("Columns sweep anisotropy -1 (left) -> +1 (right); rows sweep gloss 0.6 -> 0.9.");
    spdlog::info("Watch the round GGX highlight stretch into a brushed-metal streak that flips direction across the center column.");
    spdlog::info("Orbit: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset, Esc quit.");

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(focusPoint, std::max(gridRadius * 1.6f, 6.0f));
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

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
