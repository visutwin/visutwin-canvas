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
#include "platform/graphics/graphicsDeviceCreate.h"
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

    spdlog::info("*** VisuTwin Shadow Cascades Example Started ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Shadow Cascades Example", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    // Materials for the procedural scene
    // -----------------------------------------------------------------------
    auto* groundMaterial = new StandardMaterial();
    groundMaterial->setDiffuse(Color(0.35f, 0.50f, 0.28f)); // grass green

    auto* buildingMaterial = new StandardMaterial();
    buildingMaterial->setDiffuse(Color(0.75f, 0.72f, 0.68f)); // concrete gray

    auto* buildingDarkMaterial = new StandardMaterial();
    buildingDarkMaterial->setDiffuse(Color(0.55f, 0.52f, 0.50f)); // darker concrete

    auto* treeTrunkMaterial = new StandardMaterial();
    treeTrunkMaterial->setDiffuse(Color(0.45f, 0.30f, 0.15f)); // brown

    auto* treeCanopyMaterial = new StandardMaterial();
    treeCanopyMaterial->setDiffuse(Color(0.20f, 0.45f, 0.15f)); // dark green

    auto* mountainMaterial = new StandardMaterial();
    mountainMaterial->setDiffuse(Color(0.50f, 0.45f, 0.38f)); // rock gray-brown

    auto* cloudMaterial = new StandardMaterial();
    cloudMaterial->setDiffuse(Color(0.95f, 0.95f, 0.97f)); // white

    // -----------------------------------------------------------------------
    // Ground plane — large area to receive shadows
    // -----------------------------------------------------------------------
    auto* ground = createPrimitive(engine.get(), "plane", groundMaterial,
        0.0f, 0.0f, 0.0f, 600.0f, 1.0f, 600.0f, false, true);
    engine->root()->addChild(ground);

    // -----------------------------------------------------------------------
    // Mountain / large hill (stacked scaled boxes — simple pyramid shape)
    // -----------------------------------------------------------------------
    auto* mountainBase = createPrimitive(engine.get(), "box", mountainMaterial,
        -80.0f, 15.0f, -60.0f, 80.0f, 30.0f, 60.0f);
    engine->root()->addChild(mountainBase);
    auto* mountainMid = createPrimitive(engine.get(), "box", mountainMaterial,
        -80.0f, 40.0f, -60.0f, 55.0f, 20.0f, 40.0f);
    engine->root()->addChild(mountainMid);
    auto* mountainTop = createPrimitive(engine.get(), "cone", mountainMaterial,
        -80.0f, 65.0f, -60.0f, 30.0f, 30.0f, 30.0f);
    engine->root()->addChild(mountainTop);

    // -----------------------------------------------------------------------
    // Buildings / towers at various distances — cast long shadows
    // -----------------------------------------------------------------------
    std::mt19937 rng(42); // fixed seed for reproducibility
    std::uniform_real_distribution<float> heightDist(15.0f, 60.0f);
    std::uniform_real_distribution<float> widthDist(6.0f, 16.0f);

    struct BuildingPos { float x, z; };
    const BuildingPos buildingPositions[] = {
        // Near cluster
        { 30.0f, 20.0f }, { 50.0f, 15.0f }, { 40.0f, 40.0f }, { 25.0f, 50.0f },
        // Mid-distance cluster
        { 100.0f, -30.0f }, { 120.0f, -20.0f }, { 110.0f, -50.0f }, { 90.0f, -45.0f },
        { 130.0f, -40.0f },
        // Far cluster
        { -30.0f, 120.0f }, { -50.0f, 110.0f }, { -20.0f, 140.0f }, { -60.0f, 135.0f },
        // Scattered distant
        { 180.0f, 50.0f }, { -150.0f, -80.0f }, { 60.0f, -150.0f },
    };

    for (const auto& [bx, bz] : buildingPositions) {
        const float h = heightDist(rng);
        const float w = widthDist(rng);
        const float d = widthDist(rng);
        auto* mat = (static_cast<int>(bx + bz) % 2 == 0) ? buildingMaterial : buildingDarkMaterial;
        auto* building = createPrimitive(engine.get(), "box", mat,
            bx, h * 0.5f, bz, w, h, d);
        engine->root()->addChild(building);
    }

    // -----------------------------------------------------------------------
    // Trees — trunk (cylinder) + canopy (sphere) scattered around the scene
    // -----------------------------------------------------------------------
    struct TreePos { float x, z, trunkH, canopyR; };
    const TreePos treePositions[] = {
        // Along the path
        { 12.0f, 30.0f, 10.0f, 7.0f }, { -10.0f, 50.0f, 12.0f, 8.0f },
        { 14.0f, -20.0f, 8.0f, 6.0f }, { -12.0f, -40.0f, 11.0f, 7.0f },
        // Near buildings
        { 60.0f, 30.0f, 9.0f, 6.0f }, { 70.0f, -10.0f, 13.0f, 8.0f },
        // Mid-distance
        { -40.0f, 40.0f, 10.0f, 7.0f }, { -60.0f, 20.0f, 14.0f, 9.0f },
        { 80.0f, 80.0f, 11.0f, 7.0f }, { -80.0f, 80.0f, 10.0f, 8.0f },
        // Far trees
        { 150.0f, -100.0f, 12.0f, 8.0f }, { -120.0f, 100.0f, 15.0f, 10.0f },
        { 100.0f, 150.0f, 11.0f, 7.0f }, { -100.0f, -120.0f, 13.0f, 9.0f },
    };

    for (const auto& [tx, tz, th, cr] : treePositions) {
        auto* trunk = createPrimitive(engine.get(), "cylinder", treeTrunkMaterial,
            tx, th * 0.5f, tz, 1.5f, th, 1.5f);
        engine->root()->addChild(trunk);
        auto* canopy = createPrimitive(engine.get(), "sphere", treeCanopyMaterial,
            tx, th + cr * 0.6f, tz, cr * 2.0f, cr * 1.6f, cr * 2.0f);
        engine->root()->addChild(canopy);
    }

    // -----------------------------------------------------------------------
    // Clouds — orbital animation.
    // 16 cloud icospheres orbit around a center point at varying radii,
    // casting moving shadows on the ground below.
    // -----------------------------------------------------------------------
    constexpr int NUM_CLOUDS = 16;
    constexpr float CLOUD_SPEED = 0.2f;
    constexpr float CLOUD_CENTER_X = 0.0f;
    constexpr float CLOUD_HEIGHT = 120.0f;
    constexpr float CLOUD_CENTER_Z = 0.0f;

    std::vector<Entity*> cloudEntities;
    std::uniform_real_distribution<float> cloudScaleDist(25.0f, 50.0f);
    for (int i = 0; i < NUM_CLOUDS; ++i) {
        const float sx = cloudScaleDist(rng);
        const float sy = sx * 0.3f; // flattened vertically
        const float sz = cloudScaleDist(rng);
        auto* cloud = createPrimitive(engine.get(), "sphere", cloudMaterial,
            0.0f, CLOUD_HEIGHT, 0.0f, sx, sy, sz, true, false);
        engine->root()->addChild(cloud);
        cloudEntities.push_back(cloud);
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
        lightComp->setShadowDistance(800.0f);

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
        cameraComp->camera()->setClearColor(Color(0.55f, 0.70f, 0.90f, 1.0f));
        cameraComp->camera()->setFarClip(2000.0f);

        auto rendering = cameraComp->rendering();
        rendering.toneMapping = TONEMAP_ACES;
        cameraComp->setRendering(rendering);
    }

    camera->setPosition(Vector3(120.0f, 80.0f, 120.0f));
    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(Vector3(0.0f, 20.0f, 0.0f));
    cameraControls->setEnableFly(false);
    cameraControls->setAutoFarClip(true);
    cameraControls->setMoveSpeed(100.0f);
    cameraControls->setMoveFastSpeed(300.0f);
    cameraControls->setMoveSlowSpeed(30.0f);
    cameraControls->setOrbitDistance(200.0f);
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
                cameraControls->focus(Vector3(0.0f, 20.0f, 0.0f), 200.0f);
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

        // Animate clouds — circular orbit
        for (size_t i = 0; i < cloudEntities.size(); ++i) {
            const float radialOffset = (static_cast<float>(i) / static_cast<float>(cloudEntities.size()))
                                       * (6.24f / CLOUD_SPEED);
            const float radius = 180.0f + 80.0f * std::sin(radialOffset);
            const float cloudTime = time + radialOffset;
            cloudEntities[i]->setLocalPosition(
                CLOUD_CENTER_X + radius * std::sin(cloudTime * CLOUD_SPEED),
                CLOUD_HEIGHT,
                CLOUD_CENTER_Z + radius * std::cos(cloudTime * CLOUD_SPEED)
            );
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** VisuTwin Shadow Cascades Example Finished ***");

    return 0;
}
