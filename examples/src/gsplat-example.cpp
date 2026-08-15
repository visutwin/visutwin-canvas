// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Gaussian splatting example — port of upstream's gaussian-splatting/simple:
// a captured splat on a shadow-receiving ground plane under a PCSS directional
// light, viewed with an orbit camera. Rendered via the classic gsplat path:
// instanced screen-space EWA quads with a background CPU depth sorter for
// back-to-front blending.
//
// DEVIATION: upstream uses its own `biker` capture, whose licence PlayCanvas does
// not document. This uses a CC-BY-4.0 capture instead (see tamiya-dt03.txt), so
// the splat's own transform is fitted to that model rather than copied from
// upstream; the surrounding scene matches upstream value for value.
//
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#endif

#include <SDL3/SDL.h>
#ifdef VISUTWIN_HAS_METAL
#include <QuartzCore/QuartzCore.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/gsplat/gsplatComponent.h"
#include "framework/components/gsplat/gsplatComponentSystem.h"
#include "scene/gsplat/gsplatResource.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1200;
constexpr int WINDOW_HEIGHT = 800;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

const std::string splatPath = rootPath + "/models/tamiya-dt03.compressed.ply";

// Upstream's scene layout, kept as-is (the ground is 10x10 centred on the origin,
// so its top surface is at y = -0.45 + 0.5 = 0.05 and the subject stands on it).
constexpr float GROUND_TOP = 0.05f;
constexpr float SUBJECT_X = -1.5f;

// Fitted to this capture: a 180-degree flip about X puts it the right way up (raw
// 3DGS captures are Y-down, which is why upstream flips its biker too), and the
// scale brings the ~17.6-unit capture down to a ~2.6-unit subject so the orbit
// framing carries over. Offsets centre it and rest it on the ground.
constexpr float SPLAT_SCALE = 0.148f;

int main()
{
    log::init();
    log::set_level_debug();

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

    spdlog::info("*** Gaussian Splatting Example ***");

#ifdef VISUTWIN_HAS_METAL
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
#endif
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Gaussian Splatting",
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
#ifdef VISUTWIN_HAS_VULKAN
        | SDL_WINDOW_VULKAN
#endif
    );
    if (!window) {
        spdlog::error("SDL Window Creation Failed");
        shutdown();
        return -1;
    }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        spdlog::error("SDL Renderer Creation Failed");
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    void* swapchain = nullptr;
#ifdef VISUTWIN_HAS_METAL
    swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        spdlog::error("Unable to get render Metal layer");
        shutdown();
        return -1;
    }
#endif

    GraphicsDeviceOptions deviceOptions;
#ifdef VISUTWIN_HAS_VULKAN
    deviceOptions.backend = Backend::Vulkan;
#endif
    deviceOptions.swapChain = swapchain;
    deviceOptions.window = window;
    auto device = createGraphicsDevice(deviceOptions);
    if (!device) {
        spdlog::error("Unable to create graphics device");
        shutdown();
        return -1;
    }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<GSplatComponentSystem>();
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    // Upstream sets no environment at all — the splat carries its own colour and
    // the ground is lit by the directional light alone. Tone mapping is ACES and
    // exposure stays at the default 1.
    auto scene = engine->scene();
    scene->setToneMapping(TONEMAP_ACES);

    // -----------------------------------------------------------------------
    // Load the Gaussian splat PLY
    // -----------------------------------------------------------------------
    spdlog::info("Loading splats from '{}'...", splatPath);
    auto splatResource = GSplatResource::loadPly(splatPath, graphicsDevice);
    if (!splatResource) {
        spdlog::error("Splat PLY load failed");
        shutdown();
        return -1;
    }

    auto* modelEntity = new Entity();
    modelEntity->setName("splats");
    modelEntity->setEngine(engine.get());
    engine->root()->addChild(modelEntity);

    auto* gsplatComponent = static_cast<GSplatComponent*>(modelEntity->addComponent<GSplatComponent>());
    gsplatComponent->setResource(splatResource);

    // Flip upright and stand it on the ground at upstream's subject offset.
    // DEVIATION: upstream sets castShadows on the gsplat component; GSplatComponent
    // has no such option here, so the splat lights nothing and casts no shadow (the
    // ground still catches the light itself). Upstream notes gsplats are unlit there too.
    modelEntity->setLocalEulerAngles(180.0f, 0.0f, 0.0f);
    modelEntity->setLocalScale(SPLAT_SCALE, SPLAT_SCALE, SPLAT_SCALE);
    modelEntity->setLocalPosition(SUBJECT_X - 0.049f, GROUND_TOP + 0.443f, -0.031f);

    // Orbit target: upstream pivots one unit above its subject's base — scaled here
    // to this subject's height so the same framing reads the same.
    const Vector3 pivot(SUBJECT_X, GROUND_TOP + 0.5f, 0.0f);
    constexpr float ORBIT_DISTANCE = 4.0f;   // upstream ORBIT_DISTANCE
    constexpr float ORBIT_YAW = 32.0f;     // upstream ORBIT_INITIAL_YAW
    constexpr float ORBIT_PITCH = -10.0f;  // upstream ORBIT_INITIAL_PITCH

    // -----------------------------------------------------------------------
    // Lights
    // -----------------------------------------------------------------------
    // Single shadow-casting directional light, upstream's values verbatim.
    // DEVIATION: LightComponent has no shadowIntensity / shadowSamples /
    // shadowBlockerSamples setters, so upstream's 0.5 shadow intensity and its
    // 16/16 PCSS sample counts fall back to the engine defaults.
    auto* keyLight = new Entity();
    keyLight->setEngine(engine.get());
    auto* keyLightComp = static_cast<LightComponent*>(keyLight->addComponent<LightComponent>());
    if (keyLightComp) {
        keyLightComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        keyLightComp->setColor(Color(1.0f, 1.0f, 1.0f));
        keyLightComp->setIntensity(1.0f);
        keyLightComp->setCastShadows(true);
        keyLightComp->setShadowType(ShadowType::SHADOW_PCSS_32F);
        keyLightComp->setShadowResolution(2048);
        keyLightComp->setShadowDistance(10.0f);
        keyLightComp->setShadowBias(0.2f);
        keyLightComp->setShadowNormalBias(0.05f);
        keyLightComp->setPenumbraSize(0.05f);
        keyLightComp->setPenumbraFalloff(4.0f);
    }
    keyLight->setLocalEulerAngles(55.0f, 0.0f, 20.0f);
    engine->root()->addChild(keyLight);

    // Ground plane to receive the shadow — upstream's box, material and transform.
    // GOTCHA: on StandardMaterial the diffuse/metalness/gloss setters are the ones
    // updateUniforms() reads; setBaseColorFactor and friends get overwritten.
    auto groundMaterial = std::make_shared<StandardMaterial>();
    groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.4f));
    groundMaterial->setGloss(0.2f);
    groundMaterial->setMetalness(0.5f);
    groundMaterial->setUseMetalness(true);

    auto* ground = new Entity();
    ground->setEngine(engine.get());
    auto* groundRender = static_cast<RenderComponent*>(ground->addComponent<RenderComponent>());
    if (groundRender) {
        groundRender->setType("box");
        groundRender->setMaterial(groundMaterial.get());
        groundRender->setCastShadows(false);
        groundRender->setReceiveShadows(true);
    }
    ground->setLocalScale(10.0f, 1.0f, 10.0f);
    ground->setLocalPosition(0.0f, -0.45f, 0.0f);
    engine->root()->addChild(ground);

    // -----------------------------------------------------------------------
    // Camera with orbit controls
    // -----------------------------------------------------------------------
    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    cameraEntity->addComponent<ScriptComponent>();

    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
    }

    // Place the camera at upstream's initial orbit pose, then hand that pose to
    // CameraControls: setFocusPoint derives the orbit distance and angles from the
    // camera's CURRENT position without moving it, so the exact pose survives.
    const float yawRad = ORBIT_YAW * DEG_TO_RAD;
    const float pitchRad = ORBIT_PITCH * DEG_TO_RAD;
    cameraEntity->setLocalPosition(
        pivot.getX() + ORBIT_DISTANCE * std::cos(pitchRad) * std::sin(yawRad),
        pivot.getY() - ORBIT_DISTANCE * std::sin(pitchRad),
        pivot.getZ() + ORBIT_DISTANCE * std::cos(pitchRad) * std::cos(yawRad));
    engine->root()->addChild(cameraEntity);

    auto* cameraControls = cameraEntity->script()->create<CameraControls>();
    cameraControls->setFocusPoint(pivot);
    cameraControls->setEnableFly(false);
    cameraControls->storeResetState();

    spdlog::info("Controls: LMB/RMB orbit, Shift/MMB pan, Wheel zoom, F focus, R reset, Esc quit");

    // -----------------------------------------------------------------------
    // Main loop — the splat sorter runs on its own thread; engine->update(dt)
    // feeds it the current camera so the back-to-front order stays correct.
    // -----------------------------------------------------------------------
    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(pivot, ORBIT_DISTANCE);
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
        const float dt = static_cast<float>(dtSeconds);

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** Gaussian Splatting Example Finished ***");

    return 0;
}
