// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Clearcoat material demo (mirrors PlayCanvas materials/clear-coat).
//
// Clearcoat is a second, thin dielectric specular lobe (IOR 1.5, F0=0.04)
// layered over the standard PBR base — most visible as a sharp environment
// reflection floating on top of the base surface.
//
// Two rows of procedural spheres over a helipad env atlas + yellow directional
// light:
//   Top row (y = +1.6): a rough, dark base material. clearCoat sweeps 0 -> 1
//       left to right, so the glossy clear layer fades in from bare base
//       (left) to a fully coated surface (right).
//   Bottom row (y = -1.6): full clearcoat (clearCoat = 1). clearCoatGloss
//       sweeps 0 -> 1 left to right, so the coat goes from a dull, blurry
//       reflection (left) to a crisp mirror-like reflection (right).
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <algorithm>
#include <memory>

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "framework/assets/asset.h"
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

Entity* createSphere(Engine* engine, Material* material, const Vector3& position)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(1.15f, 1.15f, 1.15f);
    if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
        render->setMaterial(material);
        render->setType("sphere");
    }
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

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Clearcoat", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.2f, 0.2f, 0.22f);
    scene->setSkyboxMip(1);
    scene->setSkyboxIntensity(1.0f);

    // Environment atlas drives the reflections the clearcoat lobe shows off.
    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad env atlas");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    // Yellow directional light (matches the upstream example) so highlights read.
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 1.0f, 0.0f, 1.0f));
        lightComponent->setIntensity(1.0f);
    }
    light->setLocalEulerAngles(45.0f, 180.0f, 0.0f);
    engine->root()->addChild(light);

    constexpr int columns = 5;
    constexpr float spacingX = 2.7f;
    constexpr float rowTopY = 1.6f;
    constexpr float rowBottomY = -1.6f;
    const float x0 = -0.5f * spacingX * static_cast<float>(columns - 1);

    // Keep materials alive for the app lifetime.
    std::vector<std::shared_ptr<StandardMaterial>> materials;

    // --- Top row: rough dark base, clearCoat 0 -> 1 (coat fades in) ---
    for (int i = 0; i < columns; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(columns - 1);
        auto mat = std::make_shared<StandardMaterial>();
        mat->setDiffuse(Color(0.12f, 0.12f, 0.14f, 1.0f)); // dark base
        mat->setMetalness(0.0f);
        mat->setGloss(0.1f);                               // rough base (low gloss)
        mat->setClearCoat(t);                              // 0 (bare) -> 1 (fully coated)
        mat->setClearCoatGloss(1.0f);                      // mirror-smooth coat
        materials.push_back(mat);
        createSphere(engine.get(), mat.get(), Vector3(x0 + spacingX * static_cast<float>(i), rowTopY, 0.0f));
    }

    // --- Bottom row: full clearcoat, clearCoatGloss 0 -> 1 (dull -> mirror) ---
    for (int i = 0; i < columns; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(columns - 1);
        auto mat = std::make_shared<StandardMaterial>();
        mat->setDiffuse(Color(0.55f, 0.12f, 0.1f, 1.0f)); // red base like upstream
        mat->setMetalness(0.0f);
        mat->setGloss(0.4f);
        mat->setClearCoat(1.0f);                           // full coat everywhere
        mat->setClearCoatGloss(t);                         // 0 (dull) -> 1 (mirror)
        mat->setClearCoatGlossInvert(false);               // value is glossiness, not roughness
        mat->setClearCoatBumpiness(1.0f);
        materials.push_back(mat);
        createSphere(engine.get(), mat.get(), Vector3(x0 + spacingX * static_cast<float>(i), rowBottomY, 0.0f));
    }

    // Orbit camera framed on the grid.
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->addComponent<ScriptComponent>();
    camera->setPosition(Vector3(0.0f, 0.0f, 18.0f));
    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(Vector3(0.0f, 0.0f, 0.0f));
    cameraControls->setEnableFly(false);
    cameraControls->setOrbitDistance(18.0f);
    cameraControls->storeResetState();

    spdlog::info("Clearcoat demo: top row rough dark base with clearCoat 0->1 (L->R); "
                 "bottom row full clearcoat with clearCoatGloss 0->1 (dull->mirror). "
                 "Orbit: LMB/RMB orbit, Wheel zoom, R reset, Esc quit.");

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            } else if (event.type == SDL_EVENT_PINCH_UPDATE && cameraControls) {
                cameraControls->addZoomInput((event.pinch.scale - 1.0f) * 10.0f);
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
