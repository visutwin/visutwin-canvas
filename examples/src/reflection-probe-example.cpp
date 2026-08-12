// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Reflection probe demo: a chrome sphere + polished floor inside a colored
// "room" cubemap. The probe is box-projected against the room volume, so the
// floor reflects the colored walls parallax-correctly. Auto-toggles box
// projection ON/OFF every 3 s — with box OFF the cubemap acts like an infinite
// environment (direction-only), so the flat floor reflects a nearly uniform
// color; with box ON each floor point reflects the wall its ray actually hits.
// Esc quits.
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
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

// Build a colored-room cubemap: each face a distinct wall color with a bright
// centered marker + grid so the parallax shift of reflections is visible. Full
// mip chain (box-downsampled per face) so roughness maps to a mip LOD.
std::shared_ptr<Texture> makeRoomCubemap(GraphicsDevice* device, const int size)
{
    // Base color + marker color per face (0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z).
    const float wall[6][3] = {
        {0.85f, 0.15f, 0.15f},  // +X red
        {0.15f, 0.75f, 0.25f},  // -X green
        {0.9f, 0.9f, 0.95f},    // +Y ceiling (light)
        {0.1f, 0.1f, 0.12f},    // -Y floor (dark)
        {0.2f, 0.4f, 0.9f},     // +Z blue
        {0.9f, 0.8f, 0.2f},     // -Z yellow
    };

    int levels = 1;
    while ((size >> levels) >= 1) ++levels;

    TextureOptions options;
    options.name = "roomCubemap";
    options.width = static_cast<uint32_t>(size);
    options.height = static_cast<uint32_t>(size);
    options.format = PixelFormat::PIXELFORMAT_RGBA8;
    options.cubemap = true;
    options.mipmaps = true;
    options.numLevels = static_cast<uint32_t>(levels);
    options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
    options.magFilter = FilterMode::FILTER_LINEAR;
    auto texture = std::make_shared<Texture>(device, options);

    const auto toByte = [](const float v) {
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };

    for (int face = 0; face < 6; ++face) {
        // Level 0: base wall color, darker grid lines, bright centered disc.
        std::vector<uint8_t> level0(static_cast<size_t>(size) * size * 4);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float u = (x + 0.5f) / size - 0.5f;
                const float v = (y + 0.5f) / size - 0.5f;
                float r = wall[face][0], g = wall[face][1], b = wall[face][2];
                // Grid lines.
                if (((x * 8 / size) % 2) ^ ((y * 8 / size) % 2)) {
                    r *= 0.7f; g *= 0.7f; b *= 0.7f;
                }
                // Bright centered disc marker.
                if (std::sqrt(u * u + v * v) < 0.18f) {
                    r = std::min(1.0f, r + 0.6f);
                    g = std::min(1.0f, g + 0.6f);
                    b = std::min(1.0f, b + 0.6f);
                }
                uint8_t* px = &level0[(static_cast<size_t>(y) * size + x) * 4];
                px[0] = toByte(r); px[1] = toByte(g); px[2] = toByte(b); px[3] = 255;
            }
        }
        texture->setLevelData(0, level0.data(), level0.size(), static_cast<uint32_t>(face));

        // Downsample successive mips (box filter, per-face).
        std::vector<uint8_t> prev = std::move(level0);
        int prevSize = size;
        for (int level = 1; level < levels; ++level) {
            const int mipSize = std::max(1, prevSize / 2);
            std::vector<uint8_t> mip(static_cast<size_t>(mipSize) * mipSize * 4);
            for (int y = 0; y < mipSize; ++y) {
                for (int x = 0; x < mipSize; ++x) {
                    int acc[4] = {0, 0, 0, 0};
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(prevSize - 1, x * 2 + dx);
                            const int sy = std::min(prevSize - 1, y * 2 + dy);
                            const uint8_t* s = &prev[(static_cast<size_t>(sy) * prevSize + sx) * 4];
                            for (int c = 0; c < 4; ++c) acc[c] += s[c];
                        }
                    }
                    uint8_t* d = &mip[(static_cast<size_t>(y) * mipSize + x) * 4];
                    for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(acc[c] / 4);
                }
            }
            texture->setLevelData(static_cast<uint32_t>(level), mip.data(), mip.size(),
                static_cast<uint32_t>(face));
            prev = std::move(mip);
            prevSize = mipSize;
        }
    }

    texture->upload();
    return texture;
}

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

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Reflection Probe", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
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
    scene->setAmbientLight(0.18f, 0.18f, 0.2f);

    // Room bounds — the reflection probe's box volume.
    const Vector3 boxMin(-6.0f, -1.0f, -6.0f);
    const Vector3 boxMax(6.0f, 8.0f, 6.0f);
    auto roomCube = makeRoomCubemap(graphicsDevice.get(), 256);
    scene->setReflectionProbe(roomCube.get(), Vector3(0.0f, 3.0f, 0.0f), boxMin, boxMax, true, 1.0f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    if (auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>())) {
        cameraComponent->camera()->setClearColor(Color(0.08f, 0.08f, 0.1f, 1.0f));
        cameraComponent->camera()->setFov(55.0f);
    }
    camera->setLocalPosition(0.0f, 3.2f, 9.0f);
    camera->setLocalEulerAngles(-14.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lightComponent->setIntensity(1.0f);
    }
    light->setLocalEulerAngles(50.0f, -30.0f, 0.0f);
    engine->root()->addChild(light);

    // Polished floor (the parallax money shot).
    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setDiffuse(Color(0.15f, 0.15f, 0.18f, 1.0f));
    floorMaterial->setMetalness(0.9f);
    floorMaterial->setGlossInvert(true);
    floorMaterial->setGloss(0.08f);   // low roughness → sharp reflections
    engine->root()->addChild(createEntity(
        engine.get(), floorMaterial.get(), "plane", Vector3(0.0f, -1.0f, 0.0f), Vector3(12.0f, 1.0f, 12.0f)));

    // Chrome sphere.
    auto chromeMaterial = std::make_shared<StandardMaterial>();
    chromeMaterial->setDiffuse(Color(0.95f, 0.95f, 0.95f, 1.0f));
    chromeMaterial->setMetalness(1.0f);
    chromeMaterial->setGlossInvert(true);
    chromeMaterial->setGloss(0.05f);
    engine->root()->addChild(createEntity(
        engine.get(), chromeMaterial.get(), "sphere", Vector3(0.0f, 0.6f, 0.0f), Vector3(2.4f, 2.4f, 2.4f)));

    spdlog::info("Reflection probe: chrome sphere + polished floor in a colored room cubemap.");
    spdlog::info("Auto-toggles box projection ON/OFF every 3 s. Esc quits.");

    bool running = true;
    bool boxProjection = true;
    float elapsed = 0.0f;
    float toggleTimer = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    const auto applyProbe = [&]() {
        scene->setReflectionProbe(roomCube.get(), Vector3(0.0f, 3.0f, 0.0f),
            boxMin, boxMax, boxProjection, 1.0f);
        spdlog::info("Box projection: {}", boxProjection ? "ON (parallax-corrected)" : "OFF (infinite env)");
    };

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

        toggleTimer += static_cast<float>(dtSeconds);
        if (toggleTimer >= 3.0f) {
            toggleTimer = 0.0f;
            boxProjection = !boxProjection;
            applyProbe();
        }

        // Gentle camera sway so reflections read as 3D.
        camera->setLocalPosition(std::sin(elapsed * 0.3f) * 2.5f, 3.2f, 9.0f);
        camera->setLocalEulerAngles(-14.0f, std::sin(elapsed * 0.3f) * 10.0f, 0.0f);

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
