// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Gaussian splatting tier-2 demo:
//   left  : sh_sphere.ply  — uncompressed 3DGS PLY with band-3 spherical harmonics;
//           the whole shell shifts red<->blue as the camera orbits (view-dependent).
//   right : compressed_rings.ply — SuperSplat .compressed.ply (11-10-11 position/
//           scale, 2-10-10-10 rotation, 8888 color, per-256 chunk bounds), ~4x
//           smaller on disk; proves the compressed parser round-trips.
// The camera auto-orbits in azimuth. Esc quits.
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

#ifdef VISUTWIN_HAS_METAL
#include <QuartzCore/QuartzCore.hpp>
#endif

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/gsplat/gsplatComponent.h"
#include "framework/components/gsplat/gsplatComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/gsplat/gsplatResource.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

Entity* addSplats(Engine* engine, const std::string& path, const Vector3& position)
{
    auto resource = GSplatResource::loadPly(path, engine->graphicsDevice());
    if (!resource) {
        spdlog::error("Failed to load splats '{}'", path);
        return nullptr;
    }
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    auto* gsplat = static_cast<GSplatComponent*>(entity->addComponent<GSplatComponent>());
    gsplat->setResource(resource);
    engine->root()->addChild(entity);
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

#ifdef VISUTWIN_HAS_METAL
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
#endif
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "GSplat Tier 2 (SH + Compressed)", WINDOW_WIDTH, WINDOW_HEIGHT,
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
    createOptions.registerComponentSystem<GSplatComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setToneMapping(TONEMAP_NEUTRAL);
    scene->setExposure(1.4f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    if (auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>())) {
        cameraComponent->camera()->setClearColor(Color(0.03f, 0.03f, 0.05f, 1.0f));
        cameraComponent->camera()->setFov(55.0f);
    }
    engine->root()->addChild(camera);

    addSplats(engine.get(), rootPath + "/models/sh_sphere.ply", Vector3(-2.0f, 0.0f, 0.0f));
    addSplats(engine.get(), rootPath + "/models/compressed_rings.ply", Vector3(2.2f, 0.0f, 0.0f));

    spdlog::info("Left: SH sphere (view-dependent color). Right: compressed rings. Camera auto-orbits.");

    bool running = true;
    float elapsed = 0.0f;
    const Vector3 focus(0.0f, 0.0f, 0.0f);
    const float orbitRadius = 6.5f;
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

        // Auto-orbit in azimuth so the SH sphere's view-dependent color sweeps.
        const float angle = elapsed * 0.6f;
        const Vector3 eye(focus.getX() + std::sin(angle) * orbitRadius,
                          focus.getY() + 1.2f,
                          focus.getZ() + std::cos(angle) * orbitRadius);
        camera->setLocalPosition(eye.getX(), eye.getY(), eye.getZ());
        const float yawDeg = angle * 180.0f / static_cast<float>(M_PI);
        camera->setLocalEulerAngles(-10.0f, yawDeg, 0.0f);

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
