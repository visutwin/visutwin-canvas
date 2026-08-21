// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of the upstream "dithered-transparency" example.
//
// Two glass tables, both alpha-blended AND opacity-dithered, showing that the two
// strengths are independent. LEFT leaves alphaDither unset, so opacity drives both the
// blend and the dither density — the coupled behaviour every material had before.
// RIGHT sets alphaDither explicitly, so opacity drives only the blend and alphaDither
// only the dither. Both tables also dither their SHADOWS, so a half-opaque table throws
// a correspondingly thinned shadow rather than a solid one.
//
// Values are frozen at upstream's initial state (opacity 0.5, alphaDither 0.5, TAA off).
// Upstream drives them from sliders, and at these values both tables deliberately look
// identical — the difference only appears once a slider moves.
//
// DEVIATION: the shadow dither runs in the Metal shadow fragment shader, which serves both
// the PCF and VSM paths. On Vulkan the PCF shadow pass is depth-only and omits the fragment
// stage entirely, so a dithered shadow there would need one attached; this example uses VSM,
// whose fragment stage does run.
//
// @credit Low-poly Glass Table by Sketchfab, CC BY 4.0
//   https://sketchfab.com/3d-models/low-poly-glass-table-6acac6d9201e448b92dff859b6f63aad
//
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#endif

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>
#include <vector>

#ifdef VISUTWIN_HAS_METAL
#include <QuartzCore/QuartzCore.hpp>
#endif

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/constants.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Upstream's initial slider values.
constexpr float INITIAL_OPACITY = 0.5f;
constexpr float INITIAL_ALPHA_DITHER = 0.5f;
constexpr bool INITIAL_TAA = false;

const auto envAtlasAsset = std::make_unique<Asset>(
    "env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/table-mountain-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

const auto tableAsset = std::make_unique<Asset>(
    "table",
    AssetType::CONTAINER,
    rootPath + "/models/glass-table.glb"
);

const auto diffuseAsset = std::make_unique<Asset>(
    "color",
    AssetType::TEXTURE,
    rootPath + "/textures/playcanvas.png"
);

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

#ifdef VISUTWIN_HAS_METAL
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
#endif
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Dithered Transparency", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
#ifdef VISUTWIN_HAS_VULKAN
        | SDL_WINDOW_VULKAN
#endif
    );
    if (!window) { shutdown(); return -1; }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { shutdown(); return -1; }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    void* swapchain = nullptr;
#ifdef VISUTWIN_HAS_METAL
    swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) { shutdown(); return -1; }
#endif

    GraphicsDeviceOptions deviceOptions;
#ifdef VISUTWIN_HAS_VULKAN
    deviceOptions.backend = Backend::Vulkan;
#endif
    deviceOptions.swapChain = swapchain;
    deviceOptions.window = window;
    auto device = createGraphicsDevice(deviceOptions);
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

    // Setup skydome
    auto scene = engine->scene();
    if (const auto envResource = envAtlasAsset->resource()) {
        scene->setEnvAtlas(std::get<Texture*>(*envResource));
    } else {
        spdlog::error("Failed to load the table-mountain environment atlas");
    }
    scene->setSkyboxMip(2);
    scene->setExposure(4.5f);

    Texture* diffuseTexture = nullptr;
    if (const auto diffuseResource = diffuseAsset->resource()) {
        diffuseTexture = std::get<Texture*>(*diffuseResource);
    }

    // Ground plane.
    auto groundMaterial = std::make_shared<StandardMaterial>();
    groundMaterial->setDiffuse(Color(0.8f, 0.8f, 0.8f, 1.0f));
    groundMaterial->setDiffuseMap(diffuseTexture);
    {
        auto* ground = new Entity();
        ground->setEngine(engine.get());
        if (auto* render = static_cast<RenderComponent*>(ground->addComponent<RenderComponent>())) {
            render->setMaterial(groundMaterial.get());
            render->setType("plane");
        }
        ground->setLocalPosition(0.0f, 0.0f, 0.0f);
        ground->setLocalScale(60.0f, 1.0f, 30.0f);
        engine->root()->addChild(ground);
    }

    const auto tableResource = tableAsset->resource();
    if (!tableResource) {
        spdlog::error("Failed to load models/glass-table.glb");
        shutdown();
        return -1;
    }
    auto* tableContainer = std::get<ContainerResource*>(*tableResource);

    // Instantiate the glass table and collect its alpha-blended materials. Each blended
    // material is CLONED — a container's materials are shared across instantiations, so
    // without this the two tables could not be configured independently.
    std::vector<std::shared_ptr<Material>> clonedMaterials;   // keeps the clones alive
    const auto spawnTable = [&](const Vector3& position) {
        auto* entity = tableContainer->instantiateRenderEntity();
        entity->setEngine(engine.get());
        entity->setLocalScale(3.0f, 3.0f, 3.0f);
        entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
        engine->root()->addChild(entity);

        std::vector<StandardMaterial*> materials;
        for (auto* render : entity->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                auto* source = meshInstance->material();
                if (!source || !source->transparent()) {
                    continue;
                }
                auto clone = source->clone();
                meshInstance->setMaterial(clone.get());
                clonedMaterials.push_back(clone);
                if (auto* standard = dynamic_cast<StandardMaterial*>(clone.get())) {
                    materials.push_back(standard);
                }
            }
        }
        return materials;
    };

    // LEFT — alphaDither stays unset, so opacity drives both blend strength and dither
    // density, exactly like the legacy behaviour.
    auto leftMaterials = spawnTable(Vector3(-7.0f, 0.0f, 0.0f));

    // RIGHT — alphaDither set explicitly, decoupled from opacity.
    auto rightMaterials = spawnTable(Vector3(7.0f, 0.0f, 0.0f));

    // Everything except alphaDither applies to both tables, so the only difference between
    // them is that unset-vs-explicit state.
    const auto applyShared = [](const std::vector<StandardMaterial*>& materials) {
        for (auto* material : materials) {
            material->setOpacity(INITIAL_OPACITY);
            material->setOpacityDitherMode(DitherMode::DITHER_BAYER8);
            material->setOpacityShadowDitherMode(DitherMode::DITHER_BAYER8);
        }
    };
    applyShared(leftMaterials);
    applyShared(rightMaterials);
    for (auto* material : rightMaterials) {
        material->setAlphaDither(INITIAL_ALPHA_DITHER);
    }

    // Directional light casting a soft VSM shadow.
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lc->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        lc->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lc->setRange(200.0f);
        lc->setCastShadows(true);
        lc->setShadowResolution(2048);
        lc->setShadowType(ShadowType::SHADOW_VSM_16F);
        lc->setVsmBlurSize(20);
        lc->setShadowBias(0.1f);
        lc->setShadowNormalBias(0.1f);
    }
    light->setLocalEulerAngles(75.0f, 120.0f, 20.0f);
    engine->root()->addChild(light);

    // Camera.
    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    cameraEntity->addComponent<ScriptComponent>();
    cameraEntity->setLocalPosition(-14.0f, 12.0f, 20.0f);
    engine->root()->addChild(cameraEntity);

    const Vector3 focusPoint(0.0f, 4.0f, 0.0f);
    if (cameraComponent) {
        if (cameraComponent->camera()) {
            cameraComponent->camera()->setFov(70.0f);
        }
        // Upstream's CameraFrame: ACES tone mapping, a scene color map, TAA jitter 1, and
        // sharpening only while TAA is on.
        cameraComponent->requestSceneColorMap(true);   // upstream: cameraFrame.rendering.sceneColorMap
        auto taa = cameraComponent->taa();
        taa.enabled = INITIAL_TAA;
        taa.jitter = 1.0f;
        cameraComponent->setTaa(taa);
        auto rendering = cameraComponent->rendering();
        rendering.toneMapping = TONEMAP_ACES;
        rendering.sharpness = INITIAL_TAA ? 1.0f : 0.0f;
        cameraComponent->setRendering(rendering);
        cameraComponent->setToneMapping(TONEMAP_ACES);
    }
    cameraEntity->lookAt(focusPoint);

    auto* cameraControls = cameraEntity->script()->create<CameraControls>();
    cameraControls->setFocusPoint(focusPoint);
    cameraControls->setEnableFly(false);
    cameraControls->storeResetState();

    spdlog::info("Dithered transparency: left = dither coupled to opacity, right = decoupled "
                 "(alphaDither {:.2f}). Esc quits.", INITIAL_ALPHA_DITHER);

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
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            } else if (event.type == SDL_EVENT_PINCH_UPDATE && cameraControls) {
                cameraControls->addZoomInput((event.pinch.scale - 1.0f) * 10.0f);
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const auto dtSeconds = static_cast<float>(
            static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq));
        prevCounter = nowCounter;

        engine->update(dtSeconds);
        engine->render();
    }

    shutdown();
    return 0;
}
