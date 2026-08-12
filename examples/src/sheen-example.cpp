// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Sheen (fabric/cloth) material demo. Sheen is a soft, retro-reflective lobe
// that lights up at grazing angles — the characteristic velvet/satin rim glow.
// It reads best on a NON-metallic, fairly matte base with a saturated diffuse:
// the sheen adds a bright edge halo without turning the surface glossy.
//
// A grid of procedural spheres over a deep-red velvet base material:
//   - X axis (columns): sheen COLOR intensity, black (sheen off) -> bright white
//     (strong rim). Left sphere is plain matte cloth; right sphere has the
//     strongest fabric rim.
//   - Z axis (rows, back->front): sheen ROUGHNESS, 0.15 (tight, satiny highlight)
//     -> 1.0 (broad, dusty velvet glow).
// A grazing directional light rakes the grid so the rim lobe is clearly visible;
// the helipad env atlas supplies image-based ambient. Orbit the camera to watch
// the sheen rim track the grazing view/light angle.
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

// Number of spheres along each grid axis.
constexpr int NUM_COLS = 6;  // X: sheen colour intensity (black -> bright)
constexpr int NUM_ROWS = 4;  // Z: sheen roughness (tight -> broad)
constexpr float SPACING = 2.4f;

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
        "Sheen (Fabric) Material", WINDOW_WIDTH, WINDOW_HEIGHT,
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
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.18f, 0.18f, 0.2f);
    scene->setSkyboxMip(2);
    scene->setSkyboxIntensity(0.5f);

    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad env atlas");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    // Grazing directional light: a shallow pitch rakes the spheres so the
    // sheen lobe fires along the silhouette rim rather than the front face.
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 0.96f, 0.9f, 1.0f));
        lightComponent->setIntensity(2.2f);
    }
    light->setLocalEulerAngles(12.0f, -40.0f, 0.0f);  // low pitch -> grazing
    engine->root()->addChild(light);

    // Keep the materials alive for the lifetime of the run.
    std::vector<std::shared_ptr<StandardMaterial>> materials;
    materials.reserve(static_cast<size_t>(NUM_COLS) * NUM_ROWS);

    const float gridW = (NUM_COLS - 1) * SPACING;
    const float gridD = (NUM_ROWS - 1) * SPACING;

    for (int row = 0; row < NUM_ROWS; ++row) {
        // Sheen roughness increases toward the front row: tight satin -> broad velvet.
        const float rt = NUM_ROWS > 1 ? static_cast<float>(row) / (NUM_ROWS - 1) : 0.0f;
        const float sheenRoughness = 0.15f + rt * 0.85f;

        for (int col = 0; col < NUM_COLS; ++col) {
            // Sheen colour intensity increases to the right: 0 (off) -> 1 (bright rim).
            const float ct = NUM_COLS > 1 ? static_cast<float>(col) / (NUM_COLS - 1) : 0.0f;

            auto material = std::make_shared<StandardMaterial>();
            // Deep-red velvet base: non-metallic, saturated diffuse, fairly matte.
            material->setDiffuse(Color(0.55f, 0.06f, 0.09f, 1.0f));
            material->setUseMetalness(true);
            material->setMetalness(0.0f);
            material->setGloss(0.35f);  // moderate — base stays matte, sheen adds the rim

            // Warm-white fabric sheen; intensity ramps across the columns.
            material->setSheenColor(Color(ct, ct * 0.95f, ct * 0.88f, 1.0f));
            material->setSheenRoughness(sheenRoughness);

            auto* sphere = new Entity();
            sphere->setEngine(engine.get());
            const float x = col * SPACING - gridW * 0.5f;
            const float z = gridD * 0.5f - row * SPACING;
            sphere->setLocalPosition(x, 0.0f, z);
            sphere->setLocalScale(0.9f, 0.9f, 0.9f);
            if (auto* render = static_cast<RenderComponent*>(sphere->addComponent<RenderComponent>())) {
                render->setMaterial(material.get());
                render->setType("sphere");
            }
            engine->root()->addChild(sphere);
            materials.push_back(std::move(material));
        }
    }

    // Orbit camera framing the grid centre.
    const Vector3 focus(0.0f, 0.0f, 0.0f);
    const float sceneRadius = std::max(gridW, gridD) * 0.5f + 1.5f;

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->addComponent<ScriptComponent>();
    camera->setPosition(Vector3(0.0f, 4.0f, sceneRadius * 2.4f));
    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(focus);
    cameraControls->setEnableFly(false);
    cameraControls->setMoveSpeed(2.0f * sceneRadius);
    cameraControls->setMoveFastSpeed(4.0f * sceneRadius);
    cameraControls->setMoveSlowSpeed(sceneRadius);
    cameraControls->setOrbitDistance(std::max(sceneRadius * 2.4f, 12.0f));
    cameraControls->storeResetState();

    spdlog::info("Sheen fabric grid: {}x{} spheres. X = sheen colour (black->bright), "
                 "Z = sheen roughness (0.15->1.0). Orbit: LMB/RMB, wheel zoom, R reset, Esc quit.",
                 NUM_COLS, NUM_ROWS);

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
