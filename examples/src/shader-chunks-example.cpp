// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// ShaderChunks registry demo: three spheres rendered with the default chunks, then a
// GLOBAL chunk override (grayscale tonemap — affects the whole scene), then a
// PER-MATERIAL chunk override (emissive glow — affects only the middle sphere), then
// back to defaults. Cache-invalidation hashing recompiles exactly the affected
// variants; restoring defaults reuses the originally compiled programs.
// Auto-cycles phases (Space pauses, 1-4 select phase, Esc quits).
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>
#include <string>

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
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/programLibrary.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

// Global override: replaces the tonemap dispatch with a grayscale mapper. Only
// `toneMap()` is referenced by other chunks, so the override can stay minimal.
constexpr const char* GRAYSCALE_TONEMAP_CHUNK = R"(
static inline float3 toneMap(float3 color, float exposure, float mode)
{
    const float3 exposed = color * exposure;
    const float3 mapped = exposed / (exposed + float3(1.0));
    const float gray = dot(mapped, float3(0.299, 0.587, 0.114));
    return float3(gray);
}
)";

// Per-material override: adds a constant green glow on top of the material emissive.
constexpr const char* GREEN_GLOW_EMISSIVE_CHUNK = R"(
    float3 emissiveLinear = max(material.emissiveColor.rgb, float3(0.0)) + float3(0.0, 0.35, 0.05);
#if VT_FEATURE_EMISSIVE_MAP
    if (emissiveTexture.get_width() > 0 && emissiveTexture.get_height() > 0) {
        emissiveLinear *= srgbToLinear(emissiveTexture.sample(defaultSampler, uvEmissive).rgb);
    }
#endif
)";

Entity* createEntity(Engine* engine, Material* material, const char* type,
    const Vector3& position, const Vector3& scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
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
        "Shader Chunks", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.25f, 0.25f, 0.28f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->setLocalPosition(0.0f, 2.2f, 7.0f);
    camera->setLocalEulerAngles(-12.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lightComponent->setIntensity(1.3f);
    }
    light->setLocalEulerAngles(45.0f, -30.0f, 0.0f);
    engine->root()->addChild(light);

    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setDiffuse(Color(0.45f, 0.45f, 0.48f, 1.0f));
    floorMaterial->setGlossInvert(true);
    floorMaterial->setGloss(0.85f);
    engine->root()->addChild(createEntity(
        engine.get(), floorMaterial.get(), "plane", Vector3(0.0f, -0.6f, 0.0f), Vector3(24.0f, 1.0f, 24.0f)
    ));

    const Color sphereColors[3] = {
        Color(0.85f, 0.25f, 0.2f, 1.0f), Color(0.85f, 0.7f, 0.2f, 1.0f), Color(0.25f, 0.45f, 0.85f, 1.0f)
    };
    std::shared_ptr<StandardMaterial> sphereMaterials[3];
    for (int i = 0; i < 3; ++i) {
        sphereMaterials[i] = std::make_shared<StandardMaterial>();
        sphereMaterials[i]->setDiffuse(sphereColors[i]);
        sphereMaterials[i]->setGlossInvert(true);
        sphereMaterials[i]->setGloss(0.4f);
        engine->root()->addChild(createEntity(
            engine.get(), sphereMaterials[i].get(), "sphere",
            Vector3(-2.4f + 2.4f * static_cast<float>(i), 0.4f, 0.0f), Vector3(1.8f, 1.8f, 1.8f)
        ));
    }

    const auto programLibrary = getProgramLibrary(graphicsDevice);
    spdlog::info("ShaderChunks registry: {} default chunks", programLibrary->chunks().names().size());

    // Phases: 0 defaults, 1 global grayscale tonemap, 2 per-material green glow, 3 defaults again.
    const auto applyPhase = [&](const int phase) {
        programLibrary->chunks().clearOverrides();
        sphereMaterials[1]->clearShaderChunks();
        switch (phase) {
            case 1:
                programLibrary->chunks().set("common-tonemap", GRAYSCALE_TONEMAP_CHUNK);
                spdlog::info("Phase 1: GLOBAL override 'common-tonemap' (grayscale) — hash {:#x}",
                    programLibrary->chunks().hash());
                break;
            case 2:
                sphereMaterials[1]->setShaderChunk("forward-fragment-emissive", GREEN_GLOW_EMISSIVE_CHUNK);
                spdlog::info("Phase 2: PER-MATERIAL override 'forward-fragment-emissive' on middle sphere — hash {:#x}",
                    sphereMaterials[1]->shaderChunksHash());
                break;
            default:
                spdlog::info("Phase {}: default chunks", phase);
                break;
        }
    };
    applyPhase(0);

    bool running = true;
    bool autoCycle = true;
    int phase = 0;
    float cycleTimer = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    spdlog::info("Keys: 1-4 = phase, Space = auto-cycle, Esc = quit");

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key >= SDLK_1 && event.key.key <= SDLK_4) {
                autoCycle = false;
                phase = static_cast<int>(event.key.key - SDLK_1);
                applyPhase(phase);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                autoCycle = true;
                cycleTimer = 0.0f;
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (autoCycle) {
            cycleTimer += static_cast<float>(dtSeconds);
            if (cycleTimer >= 3.0f) {
                cycleTimer = 0.0f;
                phase = (phase + 1) % 4;
                applyPhase(phase);
            }
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
