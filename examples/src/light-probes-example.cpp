// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Ambient SH light-probe demo: a synthetic gradient sky (warm above, teal below) is
// projected onto 9 spherical-harmonics coefficients on the CPU and drives the ambient
// term of a sphere grid. Auto-cycles between flat ambient and SH ambient every few
// seconds (press 1 = flat, 2 = SH, Space = resume auto-cycle).
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>
#include <vector>

#include <QuartzCore/QuartzCore.hpp>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/graphics/sphericalHarmonics.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

// Synthetic equirect radiance: warm orange above the horizon, teal below, with a mild
// warm boost toward +Z so the linear SH bands pick up a lateral tint as well.
std::array<Vector3, 9> buildGradientSkySH()
{
    constexpr int width = 64;
    constexpr int height = 32;
    std::vector<float> pixels(static_cast<size_t>(width) * height * 4);

    for (int py = 0; py < height; ++py) {
        const float v = (static_cast<float>(py) + 0.5f) / height;
        const float sinTheta = std::sin((0.5f - v) * static_cast<float>(M_PI)); // +1 at top row
        const float up = std::clamp(sinTheta * 2.0f + 0.5f, 0.0f, 1.0f);        // horizon blend
        for (int px = 0; px < width; ++px) {
            const float u = (static_cast<float>(px) + 0.5f) / width;
            const float phi = (u - 0.5f) * 2.0f * static_cast<float>(M_PI);
            const float towardPlusZ = std::clamp(std::cos(phi), 0.0f, 1.0f) * std::cos((0.5f - v) * static_cast<float>(M_PI));

            const Vector3 sky(1.0f, 0.55f, 0.25f);   // warm orange
            const Vector3 ground(0.1f, 0.45f, 0.4f); // teal
            Vector3 radiance = ground * (1.0f - up) + sky * up;
            radiance += Vector3(0.3f, 0.05f, 0.0f) * towardPlusZ;

            float* p = &pixels[(static_cast<size_t>(py) * width + px) * 4];
            p[0] = radiance.getX();
            p[1] = radiance.getY();
            p[2] = radiance.getZ();
            p[3] = 1.0f;
        }
    }

    return sh::projectEquirect(pixels.data(), width, height, 4);
}

Entity* createSphereEntity(Engine* engine, Material* material, const Vector3& position, const float scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale, scale, scale);

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setMaterial(material);
        render->setType("sphere");
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

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Light Probes", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        shutdown();
        return -1;
    }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        shutdown();
        return -1;
    }

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) {
        shutdown();
        return -1;
    }

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
    scene->setToneMapping(TONEMAP_LINEAR);

    // Flat-ambient baseline matches the SH sky's average brightness so only the
    // directional gradient differs between the two modes.
    const std::array<Vector3, 9> skySH = buildGradientSkySH();
    const Vector3 flatAmbient = skySH[0];
    scene->setAmbientLight(flatAmbient.getX(), flatAmbient.getY(), flatAmbient.getZ());

    spdlog::info("SH[0] (mean irradiance): ({:.3f}, {:.3f}, {:.3f})",
        skySH[0].getX(), skySH[0].getY(), skySH[0].getZ());
    const Vector3 up = sh::evaluate(skySH, Vector3(0.0f, 1.0f, 0.0f));
    const Vector3 down = sh::evaluate(skySH, Vector3(0.0f, -1.0f, 0.0f));
    spdlog::info("SH irradiance +Y: ({:.3f}, {:.3f}, {:.3f})  -Y: ({:.3f}, {:.3f}, {:.3f})",
        up.getX(), up.getY(), up.getZ(), down.getX(), down.getY(), down.getZ());

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->setLocalPosition(0.0f, 0.0f, 6.0f);
    engine->root()->addChild(camera);

    // Pure-diffuse white material — ambient is the only light source, so the sphere
    // shading is exactly the SH irradiance of the surface normal.
    auto material = std::make_shared<StandardMaterial>();
    material->setName("probe-diffuse");
    material->setBaseColorFactor(Color(1.0f, 1.0f, 1.0f, 1.0f));
    material->setMetallicFactor(0.0f);
    material->setRoughnessFactor(1.0f);

    for (int gy = -1; gy <= 1; ++gy) {
        for (int gx = -2; gx <= 2; ++gx) {
            engine->root()->addChild(createSphereEntity(
                engine.get(), material.get(),
                Vector3(static_cast<float>(gx) * 1.3f, static_cast<float>(gy) * 1.3f, 0.0f), 1.1f
            ));
        }
    }

    bool running = true;
    bool useSH = false;
    bool autoCycle = true;
    float cycleTimer = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    const auto applyMode = [&](const bool sh) {
        useSH = sh;
        if (useSH) {
            scene->setAmbientSH(skySH);
        } else {
            scene->clearAmbientSH();
        }
        spdlog::info("Ambient mode: {}", useSH ? "SH light probes" : "flat ambient");
    };
    applyMode(false);
    spdlog::info("Keys: 1 = flat ambient, 2 = SH probes, Space = auto-cycle, Esc = quit");

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_1) {
                autoCycle = false;
                applyMode(false);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_2) {
                autoCycle = false;
                applyMode(true);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                autoCycle = true;
                cycleTimer = 0.0f;
                spdlog::info("Auto-cycle resumed");
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (autoCycle) {
            cycleTimer += static_cast<float>(dtSeconds);
            if (cycleTimer >= 3.0f) {
                cycleTimer = 0.0f;
                applyMode(!useSH);
            }
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
