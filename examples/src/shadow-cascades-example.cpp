// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Shadow cascades example — procedural cityscape scene with buildings, trees,
// and terrain features at varying distances to demonstrate CSM quality.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <random>
#include <core/shape/boundingBox.h>
#include <framework/assets/asset.h>

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/constants.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/animation/animationComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/extras/miniStats.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/metal/metalGraphicsDevice.h"
#include "viz/overlay/imguiOverlay.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1100;
constexpr int WINDOW_HEIGHT = 750;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

const auto envAtlas = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

// Low-poly terrain model (same hero asset as upstream shadow-cascades).
const auto terrainAsset = std::make_unique<Asset>(
    "terrain",
    AssetType::CONTAINER,
    rootPath + "/models/terrain.glb"
);

// Helper: create a primitive entity with material, shadows, position, and scale
Entity* createPrimitive(Engine* engine, const std::string& type, StandardMaterial* material,
                        float x, float y, float z, float sx, float sy, float sz,
                        bool castShadow = true, bool receiveShadow = true)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setType(type);
        render->setMaterial(material);
        render->setCastShadows(castShadow);
        render->setReceiveShadows(receiveShadow);
    }
    entity->setLocalPosition(x, y, z);
    entity->setLocalScale(sx, sy, sz);
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

    spdlog::info("*** Shadow Cascades Example Started ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Shadow Cascades Example", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        std::cerr << "SDL Window Creation Failed" << std::endl;
        shutdown();
        return -1;
    }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::cerr << "SDL Renderer Creation Failed" << std::endl;
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        std::cerr << "Unable to get render Metal layer" << std::endl;
        shutdown();
        return -1;
    }

    auto device = createGraphicsDevice(
        GraphicsDeviceOptions{.swapChain = swapchain, .window = window}
    );
    if (!device) {
        std::cerr << "Unable to create graphics device" << std::endl;
        shutdown();
        return -1;
    }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<AnimationComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);

    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);

    engine->start();

    // ImGui overlay + MiniStats HUD. The HUD hooks the engine's "postrender" event, so nothing
    // else has to be called per frame.
    // The overlay is implemented with imgui_impl_metal, so it only works on a Metal
    // device. Both backends can be compiled in and picked at runtime (VISUTWIN_BACKEND),
    // and the unchecked static_cast here handed ImGui_ImplMetal_Init a Vulkan device —
    // a segfault at startup that made this example unusable on Vulkan.
    ImGuiOverlay overlay;
    auto* metalDevice = dynamic_cast<MetalGraphicsDevice*>(graphicsDevice.get());
    if (metalDevice) {
        overlay.init(metalDevice, window);
    } else {
        spdlog::warn("ImGui overlay and the MiniStats HUD need a Metal device; "
            "skipping them on this backend.");
    }
    MiniStats miniStats(engine, metalDevice ? &overlay : nullptr);

    auto scene = engine->scene();

    // setup skydome
    scene->setSkyboxMip(2);
    scene->setExposure(1.2f);
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.25f, 0.28f, 0.35f);

    const auto envAtlasResource = envAtlas->resource();
    if (!envAtlasResource) {
        spdlog::error("Failed to load environment atlas texture");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*envAtlasResource));

    // -----------------------------------------------------------------------
    // Instantiate the low-poly terrain model (mirrors upstream shadow-cascades).
    // The GLB contains ground, trees and static clouds; scaled up so cascades
    // span it at varying distance.
    // -----------------------------------------------------------------------
    const auto terrainResource = terrainAsset->resource();
    if (!terrainResource || !std::holds_alternative<ContainerResource*>(*terrainResource)) {
        spdlog::error("Failed to load terrain.glb");
        shutdown();
        return -1;
    }
    auto* terrainEntity = std::get<ContainerResource*>(*terrainResource)->instantiateRenderEntity();
    if (terrainEntity) {
        terrainEntity->setEngine(engine.get());
        terrainEntity->setLocalScale(30.0f, 30.0f, 30.0f);
        engine->root()->addChild(terrainEntity);
    }

    // -----------------------------------------------------------------------
    // Directional light with cascaded shadows
    // -----------------------------------------------------------------------
    auto dirLight = new Entity();
    dirLight->setEngine(engine.get());
    auto* lightComp = static_cast<LightComponent*>(dirLight->addComponent<LightComponent>());
    if (lightComp) {
        lightComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        lightComp->setColor(Color(1.0f, 0.95f, 0.85f));
        lightComp->setIntensity(1.2f);
        lightComp->setCastShadows(true);
        lightComp->setShadowBias(0.05f);
        lightComp->setShadowNormalBias(0.5f);
        lightComp->setShadowDistance(1000.0f);

        lightComp->setNumCascades(4);
        lightComp->setShadowResolution(2048);
        lightComp->setCascadeDistribution(0.5f);
        lightComp->setCascadeBlend(5.0f);
    }
    // Low sun angle for long dramatic shadows
    dirLight->setLocalEulerAngles(25.0f, 330.0f, 0.0f);
    engine->root()->addChild(dirLight);

    // -----------------------------------------------------------------------
    // Camera
    // -----------------------------------------------------------------------
    auto camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();

    if (cameraComp && cameraComp->camera()) {
        // upstream uses a light-grey clear behind the terrain.
        cameraComp->camera()->setClearColor(Color(0.9f, 0.9f, 0.9f, 1.0f));
        cameraComp->camera()->setFarClip(2000.0f);

        auto rendering = cameraComp->rendering();
        rendering.toneMapping = TONEMAP_ACES;
        cameraComp->setRendering(rendering);

        // Volumetric fog: the low sun and cascaded shadows give pronounced light shafts.
        auto fog = cameraComp->volumetricFog();
        fog.enabled = true;
        fog.density = 0.0025f;
        fog.heightBase = 0.0f;
        fog.heightFalloff = 0.010f;
        fog.anisotropy = 0.75f;      // strong forward scattering, so the sun glows through
        fog.intensity = 3.0f;
        // The ambient term has to be in the same ballpark as the sky it replaces: extinction
        // removes the background light, and only in-scattering puts light back.
        fog.ambientColor[0] = 0.55f;
        fog.ambientColor[1] = 0.62f;
        fog.ambientColor[2] = 0.75f;
        fog.ambientIntensity = 0.45f;
        fog.maxDistance = 900.0f;
        fog.steps = 32;
        fog.scale = 0.5f;
        cameraComp->setVolumetricFog(fog);
    }

    camera->setPosition(Vector3(300.0f, 160.0f, 25.0f));
    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(Vector3(0.0f, 40.0f, 0.0f));
    cameraControls->setEnableFly(false);
    cameraControls->setAutoFarClip(true);
    cameraControls->setMoveSpeed(150.0f);
    cameraControls->setMoveFastSpeed(400.0f);
    cameraControls->setMoveSlowSpeed(40.0f);
    cameraControls->setOrbitDistance(470.0f);
    cameraControls->storeResetState();

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float time = 0.0f;

    // Cascade settings state
    int numCascades = 4;
    float cascadeDistribution = 0.5f;
    float cascadeBlend = 5.0f;
    int shadowResolution = 2048;

    auto logCascadeState = [&](const char* reason) {
        spdlog::info("CSM {}: cascades={}, resolution={}, distribution={:.2f}, blend={:.2f}",
            reason, numCascades, shadowResolution, cascadeDistribution, cascadeBlend);
    };

    spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
    spdlog::info("CSM controls:");
    spdlog::info("  1-4: set cascade count");
    spdlog::info("  D/C: increase/decrease cascade distribution");
    spdlog::info("  B/V: increase/decrease cascade blend");
    spdlog::info("  +/-: increase/decrease shadow resolution");
    logCascadeState("init");

    // No pre-stored state needed — clouds use orbital animation

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            overlay.processEvent(event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;

            // Cascade count: 1-4 keys
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_1 && lightComp) {
                numCascades = 1;
                lightComp->setNumCascades(numCascades);
                logCascadeState("count");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_2 && lightComp) {
                numCascades = 2;
                lightComp->setNumCascades(numCascades);
                logCascadeState("count");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_3 && lightComp) {
                numCascades = 3;
                lightComp->setNumCascades(numCascades);
                logCascadeState("count");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_4 && lightComp) {
                numCascades = 4;
                lightComp->setNumCascades(numCascades);
                logCascadeState("count");

            // Cascade distribution: D increase, C decrease
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_D && lightComp) {
                cascadeDistribution = std::min(1.0f, cascadeDistribution + 0.05f);
                lightComp->setCascadeDistribution(cascadeDistribution);
                logCascadeState("distribution");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_C && lightComp) {
                cascadeDistribution = std::max(0.0f, cascadeDistribution - 0.05f);
                lightComp->setCascadeDistribution(cascadeDistribution);
                logCascadeState("distribution");

            // Cascade blend: B increase, V decrease
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_B && lightComp) {
                cascadeBlend = std::min(0.2f, cascadeBlend + 0.01f);
                lightComp->setCascadeBlend(cascadeBlend);
                logCascadeState("blend");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_V && lightComp) {
                cascadeBlend = std::max(0.0f, cascadeBlend - 0.01f);
                lightComp->setCascadeBlend(cascadeBlend);
                logCascadeState("blend");

            // Shadow resolution: +/- keys
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_EQUALS && lightComp) {
                shadowResolution = std::min(4096, shadowResolution * 2);
                lightComp->setShadowResolution(shadowResolution);
                logCascadeState("resolution");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_MINUS && lightComp) {
                shadowResolution = std::max(256, shadowResolution / 2);
                lightComp->setShadowResolution(shadowResolution);
                logCascadeState("resolution");

            // Camera controls
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(Vector3(0.0f, 40.0f, 0.0f), 470.0f);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();

            // Zoom
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
        time += dt;

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** Shadow Cascades Example Finished ***");

    return 0;
}
