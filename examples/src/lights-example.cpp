// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Lights example — port of upstream graphics/lights. A statue on a large grey
// ground box, lit by one of each local light type plus a directional key light,
// every one of them animated:
//   * SPOT (white)      — orbits high above, aimed at the scene, and projects the
//                         alpha channel of heart.png as a light COOKIE.
//   * OMNI (yellow)     — orbits low and fast, casts cubemap shadows, and projects
//                         a christmas cubemap COOKIE that spins with the light.
//   * DIRECTIONAL (cyan)— the key light, sweeping its yaw, casting cascaded shadows.
// Keys 1/2/3 toggle omni/spot/directional (upstream's key order); orbit camera.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "core/math/quaternion.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/constants.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1100;
constexpr int WINDOW_HEIGHT = 750;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

const auto statueAsset = std::make_unique<Asset>(
    "statue",
    AssetType::CONTAINER,
    rootPath + "/models/statue.glb"
);

// Spot light cookie: the heart's ALPHA channel masks the beam.
const auto heartAsset = std::make_unique<Asset>(
    "heart",
    AssetType::TEXTURE,
    rootPath + "/textures/heart.png",
    AssetData{.mipmaps = true}
);

// Omni light cookie: six faces assembled into a cubemap below. Face order is the
// engine's cube convention: +X, -X, +Y, -Y, +Z, -Z.
const std::array<const char*, 6> xmasFaceFiles = {
    "xmas_posx", "xmas_negx", "xmas_posy", "xmas_negy", "xmas_posz", "xmas_negz"
};

// Assemble a cubemap from six loaded 2D face textures. DEVIATION: upstream builds
// this through a 'cubemap' Asset that references six texture assets; this port has
// no cubemap asset type, so the faces are copied into one cubemap texture here.
std::shared_ptr<Texture> makeCubemapFromFaces(GraphicsDevice* device,
                                              const std::array<Texture*, 6>& faces,
                                              const std::string& name)
{
    const Texture* first = faces[0];
    if (!first) {
        return nullptr;
    }
    const uint32_t size = first->width();
    for (const Texture* face : faces) {
        if (!face || face->width() != size || face->height() != size) {
            spdlog::error("Cubemap '{}': faces must all be square and the same size", name);
            return nullptr;
        }
    }

    TextureOptions options;
    options.name = name;
    options.width = size;
    options.height = size;
    options.format = PixelFormat::PIXELFORMAT_RGBA8;
    options.cubemap = true;
    options.mipmaps = true;
    options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
    options.magFilter = FilterMode::FILTER_LINEAR;
    auto cubemap = std::make_shared<Texture>(device, options);

    for (uint32_t face = 0; face < 6; ++face) {
        const auto* pixels = static_cast<const uint8_t*>(faces[face]->getLevel(0));
        const size_t dataSize = faces[face]->getLevelDataSize(0);
        if (!pixels || dataSize == 0) {
            spdlog::error("Cubemap '{}': face {} has no CPU-side pixel data", name, face);
            return nullptr;
        }
        cubemap->setLevelData(0, pixels, dataSize, face);
    }
    // mipmaps = true makes upload() generate the roughness chain on the GPU.
    cubemap->upload();
    return cubemap;
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

    spdlog::info("*** Lights Example Started ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Lights Example", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setAmbientLight(0.2f, 0.2f, 0.2f);

    // -----------------------------------------------------------------------
    // Cookie textures.
    // -----------------------------------------------------------------------
    Texture* heartCookie = nullptr;
    if (const auto heartResource = heartAsset->resource()) {
        heartCookie = std::get<Texture*>(*heartResource);
    } else {
        spdlog::warn("heart.png failed to load — the spot light keeps a plain beam");
    }

    // Load the six cubemap faces, then assemble them. The face assets stay alive
    // for the run; only their CPU-side level 0 is read, at assembly time.
    std::vector<std::unique_ptr<Asset>> xmasFaceAssets;
    std::array<Texture*, 6> xmasFaces{};
    bool allFacesLoaded = true;
    for (size_t i = 0; i < xmasFaceFiles.size(); ++i) {
        auto asset = std::make_unique<Asset>(
            xmasFaceFiles[i],
            AssetType::TEXTURE,
            rootPath + "/cubemaps/xmas_faces/" + xmasFaceFiles[i] + ".png"
        );
        if (const auto resource = asset->resource()) {
            xmasFaces[i] = std::get<Texture*>(*resource);
        } else {
            allFacesLoaded = false;
        }
        xmasFaceAssets.push_back(std::move(asset));
    }
    std::shared_ptr<Texture> xmasCookie;
    if (allFacesLoaded) {
        xmasCookie = makeCubemapFromFaces(graphicsDevice.get(), xmasFaces, "xmas_cubemap");
    }
    if (!xmasCookie) {
        spdlog::warn("xmas cubemap failed to build — the omni light keeps a plain falloff");
    }

    // -----------------------------------------------------------------------
    // Statue.
    // -----------------------------------------------------------------------
    const auto statueResource = statueAsset->resource();
    if (!statueResource || !std::holds_alternative<ContainerResource*>(*statueResource)) {
        spdlog::error("statue.glb failed to load");
        shutdown();
        return -1;
    }
    auto* statueContainer = std::get<ContainerResource*>(*statueResource);
    auto* statue = statueContainer ? statueContainer->instantiateRenderEntity() : nullptr;
    if (!statue) {
        spdlog::error("statue.glb instantiate failed");
        shutdown();
        return -1;
    }
    statue->setEngine(engine.get());
    engine->root()->addChild(statue);

    // -----------------------------------------------------------------------
    // Camera. Upstream authors it at (0, 15, 35) and its orbit script pivots on
    // the AABB centre of everything renderable at script-init time — which is the
    // statue alone, since the ground is added after the camera. That AABB centre
    // (verified against the running reference) sits at (0.17, 7.52, 0.02), which
    // is what puts the statue where the reference frames it; orbiting the origin
    // instead tilts the whole scene up the frame.
    // -----------------------------------------------------------------------
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();
    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
    }
    camera->setPosition(Vector3(0.0f, 15.0f, 35.0f));
    engine->root()->addChild(camera);

    const Vector3 statueCenter(0.173f, 7.523f, 0.018f);
    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(statueCenter);
    cameraControls->setEnableFly(false);
    cameraControls->setZoomRange(Vector2(1.0f, 500.0f));
    cameraControls->storeResetState();

    // -----------------------------------------------------------------------
    // Ground.
    // -----------------------------------------------------------------------
    auto groundMaterial = std::make_shared<StandardMaterial>();
    groundMaterial->setName("ground");
    groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.5f, 1.0f));
    groundMaterial->setUseMetalness(true);
    groundMaterial->setMetalness(0.5f);
    groundMaterial->setGloss(0.5f);

    auto* ground = new Entity();
    ground->setEngine(engine.get());
    if (auto* render = static_cast<RenderComponent*>(ground->addComponent<RenderComponent>())) {
        render->setType("box");
        render->setMaterial(groundMaterial.get());
        render->setCastShadows(true);
        render->setReceiveShadows(true);
    }
    ground->setLocalScale(70.0f, 1.0f, 70.0f);
    ground->setLocalPosition(0.0f, -0.5f, 0.0f);
    engine->root()->addChild(ground);

    // -----------------------------------------------------------------------
    // 1. SPOT — white, heart-alpha cookie, 2D shadows.
    // -----------------------------------------------------------------------
    auto* spotLight = new Entity();
    spotLight->setEngine(engine.get());
    auto* spotComp = static_cast<LightComponent*>(spotLight->addComponent<LightComponent>());
    if (spotComp) {
        spotComp->setType(LightType::LIGHTTYPE_SPOT);
        spotComp->setColor(Color(1.0f, 1.0f, 1.0f));
        spotComp->setIntensity(0.8f);
        spotComp->setInnerConeAngle(30.0f);
        spotComp->setOuterConeAngle(31.0f);
        spotComp->setRange(100.0f);
        spotComp->setCastShadows(true);
        spotComp->setShadowBias(0.05f);
        spotComp->setShadowNormalBias(0.03f);
        spotComp->setShadowResolution(2048);
        spotComp->setCookie(heartCookie);
        spotComp->setCookieChannel(CookieChannel::COOKIE_CHANNEL_A);
        spotComp->setCookieIntensity(1.0f);
    }
    engine->root()->addChild(spotLight);

    // Emissive cone marking the light itself.
    auto coneMaterial = std::make_shared<StandardMaterial>();
    coneMaterial->setName("spot-marker");
    coneMaterial->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
    auto* cone = new Entity();
    cone->setEngine(engine.get());
    if (auto* render = static_cast<RenderComponent*>(cone->addComponent<RenderComponent>())) {
        render->setType("cone");
        render->setMaterial(coneMaterial.get());
        render->setCastShadows(false);
    }
    spotLight->addChild(cone);

    // -----------------------------------------------------------------------
    // 2. OMNI — yellow, christmas cubemap cookie, cubemap shadows.
    // -----------------------------------------------------------------------
    auto* omniLight = new Entity();
    omniLight->setEngine(engine.get());
    auto* omniComp = static_cast<LightComponent*>(omniLight->addComponent<LightComponent>());
    if (omniComp) {
        omniComp->setType(LightType::LIGHTTYPE_OMNI);
        omniComp->setColor(Color(1.0f, 1.0f, 0.0f));
        omniComp->setIntensity(0.8f);
        omniComp->setRange(111.0f);
        omniComp->setCastShadows(true);
        omniComp->setShadowBias(0.05f);
        omniComp->setShadowNormalBias(0.03f);
        omniComp->setShadowType(SHADOW_PCF3_32F);
        omniComp->setShadowResolution(256);
        omniComp->setCookie(xmasCookie.get());
        omniComp->setCookieChannel(CookieChannel::COOKIE_CHANNEL_RGB);
        omniComp->setCookieIntensity(1.0f);
    }
    // Upstream puts the marker sphere on the light entity itself.
    auto omniMarkerMaterial = std::make_shared<StandardMaterial>();
    omniMarkerMaterial->setName("omni-marker");
    omniMarkerMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
    omniMarkerMaterial->setEmissive(Color(1.0f, 1.0f, 0.0f, 1.0f));
    if (auto* render = static_cast<RenderComponent*>(omniLight->addComponent<RenderComponent>())) {
        render->setType("sphere");
        render->setMaterial(omniMarkerMaterial.get());
        render->setCastShadows(false);
    }
    engine->root()->addChild(omniLight);

    // -----------------------------------------------------------------------
    // 3. DIRECTIONAL — cyan key light with cascaded shadows.
    // -----------------------------------------------------------------------
    auto* dirLight = new Entity();
    dirLight->setEngine(engine.get());
    auto* dirComp = static_cast<LightComponent*>(dirLight->addComponent<LightComponent>());
    if (dirComp) {
        dirComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        dirComp->setColor(Color(0.0f, 1.0f, 1.0f));
        dirComp->setIntensity(0.8f);
        dirComp->setRange(100.0f);
        dirComp->setShadowDistance(50.0f);
        dirComp->setCastShadows(true);
        dirComp->setShadowBias(0.1f);
        dirComp->setShadowNormalBias(0.2f);
    }
    engine->root()->addChild(dirLight);

    spdlog::info("Keys: 1=OMNI(yellow, cubemap cookie)  2=SPOT(white, heart cookie)  3=DIRECTIONAL(cyan)");
    spdlog::info("      F focus | R reset | Esc quit | LMB/RMB orbit, Shift/MMB pan, Wheel zoom");

    const auto toggle = [](LightComponent* c, const char* name) {
        if (!c) return;
        c->setEnabled(!c->enabled());
        spdlog::info("{}: {}", name, c->enabled() ? "ON" : "OFF");
    };

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float angleRad = 1.0f;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_1) {
                toggle(omniComp, "OMNI");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_2) {
                toggle(spotComp, "SPOT");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_3) {
                toggle(dirComp, "DIRECTIONAL");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(statueCenter, 35.772f);
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
        const float dt = static_cast<float>(dtSeconds);

        angleRad += 0.3f * dt;

        // Spot: aim at (0, -5, 0), then roll the node so its -Y (the emission
        // axis) points down the view direction, then move it. Upstream aims
        // before it moves, so the aim trails the position by one frame — kept
        // as-is, since the lag is what the reference animation shows.
        spotLight->lookAt(Vector3(0.0f, -5.0f, 0.0f));
        spotLight->rotateLocal(90.0f, 0.0f, 0.0f);
        spotLight->setLocalPosition(15.0f * std::sin(angleRad), 25.0f, 15.0f * std::cos(angleRad));

        // Omni: a faster counter-rotating orbit, spinning about its own Y so the
        // projected cubemap cookie sweeps across the scene.
        omniLight->setLocalPosition(5.0f * std::sin(-2.0f * angleRad), 10.0f,
                                    5.0f * std::cos(-2.0f * angleRad));
        // Upstream uses the world-space rotate(); the light is a root child with
        // no parent rotation, so a local rotation is the same thing.
        omniLight->rotateLocal(0.0f, 50.0f * dt, 0.0f);

        dirLight->setLocalEulerAngles(45.0f, -60.0f * angleRad, 0.0f);

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** Lights Example Finished ***");

    return 0;
}
