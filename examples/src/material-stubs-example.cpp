// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Demo of the four formerly-stubbed material features. Four spheres, left to right:
//   1. spec-gloss  — white diffuse + red specular color (KHR spec-gloss model)
//   2. Oren-Nayar  — rough clay diffuse (flatter falloff than Lambert)
//   3. detail normals — procedural high-frequency normal overlay
//   4. displacement  — vertex displacement by a procedural radial-wave height map
// Phase A (0-3 s) renders all features OFF (plain PBR); phase B (3-6 s) turns them
// all ON — the self-test pixel-diffs each sphere between phases.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <functional>
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

std::shared_ptr<Texture> makeTexture(GraphicsDevice* device, const char* name, const int size,
    const std::function<void(int, int, uint8_t*)>& fill)
{
    TextureOptions options;
    options.name = name;
    options.width = static_cast<uint32_t>(size);
    options.height = static_cast<uint32_t>(size);
    options.format = PixelFormat::PIXELFORMAT_RGBA8;
    options.mipmaps = false;
    options.minFilter = FilterMode::FILTER_LINEAR;
    options.magFilter = FilterMode::FILTER_LINEAR;
    auto texture = std::make_shared<Texture>(device, options);
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            fill(x, y, &pixels[(static_cast<size_t>(y) * size + x) * 4]);
        }
    }
    texture->setLevelData(0, pixels.data(), pixels.size());
    texture->upload();
    return texture;
}

Entity* createSphere(Engine* engine, Material* material, const Vector3& position)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(1.9f, 1.9f, 1.9f);
    if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
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
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        SDL_Quit();
    };

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Material Stubs", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    scene->setAmbientLight(0.22f, 0.22f, 0.25f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->setLocalPosition(0.0f, 1.2f, 11.5f);
    camera->setLocalEulerAngles(-6.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lightComponent->setIntensity(1.4f);
    }
    light->setLocalEulerAngles(40.0f, -35.0f, 0.0f);
    engine->root()->addChild(light);

    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setDiffuse(Color(0.4f, 0.4f, 0.43f, 1.0f));
    floorMaterial->setGlossInvert(true);
    floorMaterial->setGloss(0.85f);
    {
        auto* floor = new Entity();
        floor->setEngine(engine.get());
        floor->setLocalPosition(0.0f, -1.2f, 0.0f);
        floor->setLocalScale(24.0f, 1.0f, 24.0f);
        if (auto* render = static_cast<RenderComponent*>(floor->addComponent<RenderComponent>())) {
            render->setMaterial(floorMaterial.get());
            render->setType("plane");
        }
        engine->root()->addChild(floor);
    }

    // Procedural detail-normal map: hex-ish bump field encoded as tangent normals.
    auto detailNormalTex = makeTexture(graphicsDevice.get(), "detailNormals", 128,
        [](const int x, const int y, uint8_t* px) {
            const float fx = static_cast<float>(x) * 0.5f;
            const float fy = static_cast<float>(y) * 0.5f;
            const float dhdx = std::cos(fx) * 0.9f;
            const float dhdy = std::cos(fy * 1.3f) * 0.9f;
            const float inv = 1.0f / std::sqrt(dhdx * dhdx + dhdy * dhdy + 1.0f);
            px[0] = static_cast<uint8_t>(((-dhdx * inv) * 0.5f + 0.5f) * 255.0f);
            px[1] = static_cast<uint8_t>(((-dhdy * inv) * 0.5f + 0.5f) * 255.0f);
            px[2] = static_cast<uint8_t>((inv * 0.5f + 0.5f) * 255.0f);
            px[3] = 255;
        });

    // Procedural displacement map: radial waves.
    auto displacementTex = makeTexture(graphicsDevice.get(), "displacement", 128,
        [](const int x, const int y, uint8_t* px) {
            const float u = static_cast<float>(x) / 127.0f - 0.5f;
            const float v = static_cast<float>(y) / 127.0f - 0.5f;
            const float wave = std::sin(std::sqrt(u * u + v * v) * 50.0f) * 0.5f + 0.5f;
            const auto value = static_cast<uint8_t>(wave * 255.0f);
            px[0] = px[1] = px[2] = value;
            px[3] = 255;
        });

    // 1. Spec-gloss: red specular on white diffuse (diffuse dims via energy conservation).
    auto specGlossMaterial = std::make_shared<StandardMaterial>();
    specGlossMaterial->setDiffuse(Color(0.9f, 0.9f, 0.9f, 1.0f));
    specGlossMaterial->setSpecularColor(Color(1.0f, 0.1f, 0.05f, 1.0f));
    specGlossMaterial->setGlossiness(0.6f);

    // 2. Oren-Nayar: rough clay.
    auto orenNayarMaterial = std::make_shared<StandardMaterial>();
    orenNayarMaterial->setDiffuse(Color(0.75f, 0.45f, 0.3f, 1.0f));
    orenNayarMaterial->setGlossInvert(true);
    orenNayarMaterial->setGloss(0.95f);  // very rough

    // 3. Detail normals.
    auto detailMaterial = std::make_shared<StandardMaterial>();
    detailMaterial->setDiffuse(Color(0.4f, 0.6f, 0.75f, 1.0f));
    detailMaterial->setGlossInvert(true);
    detailMaterial->setGloss(0.3f);
    detailMaterial->setDetailNormalScale(1.5f);
    detailMaterial->setDetailNormalTransform({Vector2(3.0f, 3.0f), Vector2(0.0f, 0.0f), 0.0f});

    // 4. Displacement.
    auto displacementMaterial = std::make_shared<StandardMaterial>();
    displacementMaterial->setDiffuse(Color(0.5f, 0.72f, 0.4f, 1.0f));
    displacementMaterial->setGlossInvert(true);
    displacementMaterial->setGloss(0.6f);
    displacementMaterial->setDisplacementScale(0.12f);
    displacementMaterial->setDisplacementBias(0.5f);

    engine->root()->addChild(createSphere(engine.get(), specGlossMaterial.get(), Vector3(-4.2f, 0.0f, 0.0f)));
    engine->root()->addChild(createSphere(engine.get(), orenNayarMaterial.get(), Vector3(-1.4f, 0.0f, 0.0f)));
    engine->root()->addChild(createSphere(engine.get(), detailMaterial.get(), Vector3(1.4f, 0.0f, 0.0f)));
    engine->root()->addChild(createSphere(engine.get(), displacementMaterial.get(), Vector3(4.2f, 0.0f, 0.0f)));

    const auto applyFeatures = [&](const bool on) {
        specGlossMaterial->setUseSpecGloss(on);
        orenNayarMaterial->setUseOrenNayar(on);
        detailMaterial->setDetailNormalMap(on ? detailNormalTex.get() : nullptr);
        displacementMaterial->setDisplacementMap(on ? displacementTex.get() : nullptr);
        spdlog::info("Features {}: specGloss, orenNayar, detailNormals, displacement", on ? "ON" : "OFF");
    };
    applyFeatures(false);

    spdlog::info("Phase A (features OFF) 0-3s, phase B (ON) 3-6s; Esc = quit");

    bool running = true;
    bool featuresOn = false;
    float elapsed = 0.0f;
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

        if (!featuresOn && elapsed > 3.0f) {
            featuresOn = true;
            applyFeatures(true);
        }
        if (featuresOn && elapsed > 6.0f) {
            featuresOn = false;
            elapsed = 0.0f;
            applyFeatures(false);
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
