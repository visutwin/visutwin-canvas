// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Lights example (parity with upstream graphics/lights): a procedural scene —
// a large ground plane with a cluster of primitive objects — lit by ONE of each
// light type the engine supports:
//   * DIRECTIONAL (cyan)   — the key light, casts cascaded shadows.
//   * OMNI / POINT (yellow) — animated in a fast horizontal circle, with an
//                             emissive marker sphere so you can see its position.
//   * SPOT (magenta)       — sweeps a slow larger circle, aims back at the origin,
//                             with an emissive cone marker showing the beam.
//   * AREA_RECT (green)    — a warm-green rectangular LTC panel above the cluster,
//                             with an emissive slab marker.
// The omni and spot positions are animated every frame so their falloff sweeps
// across the objects. Keys 1-4 toggle each light on/off (logged); orbit camera.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>

#include "core/math/quaternion.h"

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/constants.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponentSystem.h"
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

// Environment atlas (image-based lighting + faint skydome), same asset the sibling
// examples use. Optional — provides a little ambient specular so the metal reads.
const auto envAtlas = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

// Helper: create a primitive entity with material, shadows, position, and scale.
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
    scene->setToneMapping(TONEMAP_ACES);
    scene->setExposure(1.0f);
    // Keep ambient low so each light's contribution is clearly visible.
    scene->setAmbientLight(0.08f, 0.08f, 0.10f);
    scene->setSkyboxMip(2);
    scene->setSkyboxIntensity(0.25f);

    const auto envAtlasResource = envAtlas->resource();
    if (envAtlasResource) {
        scene->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
    } else {
        spdlog::warn("Env atlas failed to load — continuing without IBL");
    }

    // -----------------------------------------------------------------------
    // Ground plane (grey, slightly glossy metal so specular highlights read).
    // -----------------------------------------------------------------------
    auto groundMaterial = std::make_shared<StandardMaterial>();
    groundMaterial->setName("ground");
    groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.5f, 1.0f));
    groundMaterial->setUseMetalness(true);
    groundMaterial->setMetalness(0.3f);
    groundMaterial->setGloss(0.5f);
    engine->root()->addChild(createPrimitive(
        engine.get(), "plane", groundMaterial.get(),
        0.0f, 0.0f, 0.0f, 40.0f, 1.0f, 40.0f,
        /*cast*/ false, /*receive*/ true
    ));

    // -----------------------------------------------------------------------
    // Cluster of objects to be lit (near-white matte so light colors show).
    // -----------------------------------------------------------------------
    auto objectMaterial = std::make_shared<StandardMaterial>();
    objectMaterial->setName("object");
    objectMaterial->setDiffuse(Color(0.85f, 0.85f, 0.85f, 1.0f));
    objectMaterial->setUseMetalness(true);
    objectMaterial->setMetalness(0.1f);
    objectMaterial->setGloss(0.4f);

    engine->root()->addChild(createPrimitive(engine.get(), "sphere",   objectMaterial.get(),  -4.0f, 1.0f, -3.0f, 2.0f, 2.0f, 2.0f));
    engine->root()->addChild(createPrimitive(engine.get(), "box",      objectMaterial.get(),   3.5f, 1.0f, -2.0f, 2.0f, 2.0f, 2.0f));
    engine->root()->addChild(createPrimitive(engine.get(), "cylinder", objectMaterial.get(),   0.0f, 1.2f,  2.5f, 1.6f, 2.4f, 1.6f));
    engine->root()->addChild(createPrimitive(engine.get(), "cone",     objectMaterial.get(),  -2.0f, 1.0f,  4.0f, 1.6f, 2.0f, 1.6f));
    engine->root()->addChild(createPrimitive(engine.get(), "sphere",   objectMaterial.get(),   5.0f, 0.8f,  3.5f, 1.4f, 1.4f, 1.4f));

    // -----------------------------------------------------------------------
    // 1. DIRECTIONAL — cyan key light with cascaded shadows.
    // -----------------------------------------------------------------------
    auto* dirLight = new Entity();
    dirLight->setEngine(engine.get());
    auto* dirComp = static_cast<LightComponent*>(dirLight->addComponent<LightComponent>());
    if (dirComp) {
        dirComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        dirComp->setColor(Color(0.2f, 0.9f, 1.0f));   // cyan
        dirComp->setIntensity(1.0f);
        dirComp->setCastShadows(true);
        dirComp->setShadowBias(0.05f);
        dirComp->setShadowNormalBias(0.2f);
        dirComp->setShadowDistance(80.0f);
        dirComp->setNumCascades(3);
        dirComp->setShadowResolution(2048);
    }
    dirLight->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
    engine->root()->addChild(dirLight);

    // -----------------------------------------------------------------------
    // 2. OMNI / POINT — yellow, animated in a fast circle, emissive marker sphere.
    // -----------------------------------------------------------------------
    auto* omniLight = new Entity();
    omniLight->setEngine(engine.get());
    auto* omniComp = static_cast<LightComponent*>(omniLight->addComponent<LightComponent>());
    if (omniComp) {
        omniComp->setType(LightType::LIGHTTYPE_OMNI);
        omniComp->setColor(Color(1.0f, 0.85f, 0.1f));  // yellow
        omniComp->setIntensity(4.0f);
        omniComp->setRange(18.0f);
    }
    engine->root()->addChild(omniLight);

    // Emissive marker sphere so the omni position is visible.
    auto omniMarkerMat = std::make_shared<StandardMaterial>();
    omniMarkerMat->setName("omni-marker");
    omniMarkerMat->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
    omniMarkerMat->setEmissive(Color(1.0f, 0.85f, 0.1f, 1.0f));
    omniMarkerMat->setEmissiveIntensity(2.0f);
    auto* omniMarker = createPrimitive(
        engine.get(), "sphere", omniMarkerMat.get(),
        0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f, /*cast*/ false, /*receive*/ false
    );
    omniLight->addChild(omniMarker);

    // -----------------------------------------------------------------------
    // 3. SPOT — magenta, sweeps a slow larger circle, aims at origin, cone marker.
    // -----------------------------------------------------------------------
    auto* spotLight = new Entity();
    spotLight->setEngine(engine.get());
    auto* spotComp = static_cast<LightComponent*>(spotLight->addComponent<LightComponent>());
    if (spotComp) {
        spotComp->setType(LightType::LIGHTTYPE_SPOT);
        spotComp->setColor(Color(1.0f, 0.2f, 0.8f));   // magenta
        spotComp->setIntensity(6.0f);
        spotComp->setRange(40.0f);
        spotComp->setInnerConeAngle(18.0f);
        spotComp->setOuterConeAngle(28.0f);
        spotComp->setCastShadows(true);
        spotComp->setShadowBias(0.2f);
        spotComp->setShadowNormalBias(0.05f);
        spotComp->setShadowResolution(2048);
    }
    engine->root()->addChild(spotLight);

    // Emissive cone marker (small, tip along -Y = emission direction).
    auto spotMarkerMat = std::make_shared<StandardMaterial>();
    spotMarkerMat->setName("spot-marker");
    spotMarkerMat->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
    spotMarkerMat->setEmissive(Color(1.0f, 0.2f, 0.8f, 1.0f));
    spotMarkerMat->setEmissiveIntensity(2.0f);
    auto* spotMarker = createPrimitive(
        engine.get(), "cone", spotMarkerMat.get(),
        0.0f, 0.0f, 0.0f, 0.8f, 1.2f, 0.8f, /*cast*/ false, /*receive*/ false
    );
    spotLight->addChild(spotMarker);

    // -----------------------------------------------------------------------
    // 4. AREA_RECT — warm-green LTC panel above the cluster, emissive slab marker.
    //    Entity -Y is the emission direction; tilt it to face down at the scene.
    // -----------------------------------------------------------------------
    auto* areaLight = new Entity();
    areaLight->setEngine(engine.get());
    auto* areaComp = static_cast<LightComponent*>(areaLight->addComponent<LightComponent>());
    if (areaComp) {
        areaComp->setType(LightType::LIGHTTYPE_AREA_RECT);
        areaComp->setAreaShape(AreaLightShape::LIGHTSHAPE_RECT);
        areaComp->setColor(Color(0.3f, 1.0f, 0.4f));   // green
        areaComp->setIntensity(2.0f);
        areaComp->setRange(30.0f);
        areaComp->setAreaWidth(6.0f);
        areaComp->setAreaHeight(2.0f);
    }
    areaLight->setLocalPosition(0.0f, 8.0f, -6.0f);
    areaLight->setLocalEulerAngles(60.0f, 0.0f, 0.0f);
    engine->root()->addChild(areaLight);

    // Emissive slab marking the emitting panel.
    auto areaMarkerMat = std::make_shared<StandardMaterial>();
    areaMarkerMat->setName("area-marker");
    areaMarkerMat->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
    areaMarkerMat->setEmissive(Color(0.3f, 1.0f, 0.4f, 1.0f));
    areaMarkerMat->setEmissiveIntensity(3.0f);
    auto* areaMarker = createPrimitive(
        engine.get(), "box", areaMarkerMat.get(),
        0.0f, 0.0f, 0.0f, 6.0f, 0.05f, 2.0f, /*cast*/ false, /*receive*/ false
    );
    areaLight->addChild(areaMarker);

    // -----------------------------------------------------------------------
    // Orbit camera.
    // -----------------------------------------------------------------------
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();
    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setClearColor(Color(0.06f, 0.07f, 0.09f, 1.0f));
        cameraComp->camera()->setFarClip(500.0f);
        auto rendering = cameraComp->rendering();
        rendering.toneMapping = TONEMAP_ACES;
        cameraComp->setRendering(rendering);
    }
    camera->setPosition(Vector3(0.0f, 12.0f, 22.0f));
    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(Vector3(0.0f, 1.5f, 0.0f));
    cameraControls->setEnableFly(false);
    cameraControls->setOrbitDistance(26.0f);
    cameraControls->storeResetState();

    spdlog::info("Lights: 1=DIRECTIONAL(cyan/shadows)  2=OMNI(yellow)  3=SPOT(magenta)  4=AREA_RECT(green)");
    spdlog::info("Keys: 1/2/3/4 toggle each light on/off | F focus | R reset | Esc quit");
    spdlog::info("Orbit: LMB/RMB orbit, Shift/MMB pan, Wheel zoom");

    const auto toggle = [](LightComponent* c, const char* name) {
        if (!c) return;
        c->setEnabled(!c->enabled());
        spdlog::info("{}: {}", name, c->enabled() ? "ON" : "OFF");
    };

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float time = 0.0f;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_1) {
                toggle(dirComp, "DIRECTIONAL");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_2) {
                toggle(omniComp, "OMNI");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_3) {
                toggle(spotComp, "SPOT");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_4) {
                toggle(areaComp, "AREA_RECT");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(Vector3(0.0f, 1.5f, 0.0f), 26.0f);
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
        time += dt;

        // Animate the omni light: fast horizontal circle at mid height.
        const float omniAngle = time * 1.6f;
        omniLight->setLocalPosition(6.0f * std::sin(omniAngle), 4.0f, 6.0f * std::cos(omniAngle));

        // Animate the spot light: slow larger circle high above, aimed at origin.
        const float spotAngle = time * 0.6f;
        const Vector3 spotPos(12.0f * std::sin(spotAngle), 14.0f, 12.0f * std::cos(spotAngle));
        spotLight->setLocalPosition(spotPos.getX(), spotPos.getY(), spotPos.getZ());
        // Aim the node's -Y axis (the spot emission direction) at the scene
        // centre. Build the rotation that takes the reference -Y onto the
        // target direction (robust, no euler-order assumptions; no lookAt in
        // the engine API).
        const Vector3 aimDir = (Vector3(0.0f, 1.0f, 0.0f) - spotPos).normalized();
        const Vector3 ref(0.0f, -1.0f, 0.0f);
        const float d = std::clamp(ref.dot(aimDir), -1.0f, 1.0f);
        Vector3 axis = ref.cross(aimDir);
        const float axisLen = axis.length();
        if (axisLen > 1e-5f) {
            axis = axis.normalized();
            const float angleDeg = std::acos(d) * 57.29578f;
            spotLight->setLocalRotation(Quaternion::fromAxisAngle(axis, angleDeg));
        } else if (d < 0.0f) {
            // Antiparallel (aim straight up): 180° about X.
            spotLight->setLocalRotation(Quaternion::fromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 180.0f));
        } else {
            spotLight->setLocalRotation(Quaternion());
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** Lights Example Finished ***");

    return 0;
}
