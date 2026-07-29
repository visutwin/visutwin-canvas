// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// PCSS + opacity-dither demo (aligned with upstream dithered-transparency).
// The glass-table.glb model is the hero: its materials are rendered with Bayer8
// screen-door opacity dithering (opaque pass, correct depth) instead of alpha
// blending, lit by the table-mountain environment atlas. A soft-shadow ground
// plane shows contact-hardening: with PCSS the table legs' shadows are razor sharp
// at their base and soften with distance; with PCF they are uniformly sharp.
// Two half-transparent spheres flank the table to compare transparency modes:
// LEFT uses Bayer8 opacity dithering, RIGHT uses classic alpha blending.
// Auto-cycles PCF <-> PCSS every few seconds (1 = PCF, 2 = PCSS, Space = auto).
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>

#include <QuartzCore/QuartzCore.hpp>

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
#include "framework/constants.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// table-mountain environment atlas (image-based lighting for the glass table).
const auto envAtlasAsset = std::make_unique<Asset>(
    "table-mountain-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/table-mountain-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

// Low-poly glass table (Sketchfab, CC BY 4.0) — the dithered-opacity hero object.
const auto glassTableAsset = std::make_unique<Asset>(
    "glass-table",
    AssetType::CONTAINER,
    rootPath + "/models/glass-table.glb"
);

Entity* createEntity(Engine* engine, Material* material, const char* type,
    const Vector3& position, const Vector3& scale, const bool castShadows = true)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setMaterial(material);
        render->setType(type);
        render->setCastShadows(castShadows);
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
        "VisuTwin PCSS + Opacity Dither", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    scene->setAmbientLight(0.22f, 0.22f, 0.25f);

    // Image-based lighting from the table-mountain environment atlas (mirrors upstream).
    const auto envAtlasResource = envAtlasAsset->resource();
    if (envAtlasResource) {
        scene->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
        scene->setSkyboxMip(2);
        scene->setExposure(2.5f);
    } else {
        spdlog::error("Failed to load table-mountain env atlas");
    }

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->setLocalPosition(0.0f, 7.0f, 14.0f);
    camera->setLocalEulerAngles(-25.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    auto* light = new Entity();
    light->setEngine(engine.get());
    auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>());
    if (lightComponent) {
        lightComponent->setColor(Color(1.0f, 0.97f, 0.9f, 1.0f));
        lightComponent->setIntensity(1.5f);
        lightComponent->setCastShadows(true);
        lightComponent->setShadowDistance(40.0f);
        lightComponent->setNumCascades(1);
        lightComponent->setShadowResolution(2048);
        lightComponent->setShadowBias(0.05f);
        lightComponent->setShadowNormalBias(0.5f);
        lightComponent->setPenumbraSize(0.04f);  // upstream soft-shadow examples use 0.02-0.05
        lightComponent->setPenumbraFalloff(1.0f);
    }
    // Light from the upper right, so shadows stretch left across the visible ground.
    light->setLocalEulerAngles(50.0f, 115.0f, 0.0f);
    engine->root()->addChild(light);

    // Ground.
    auto groundMaterial = std::make_shared<StandardMaterial>();
    groundMaterial->setDiffuse(Color(0.55f, 0.55f, 0.58f, 1.0f));
    groundMaterial->setGlossInvert(true);
    groundMaterial->setGloss(0.9f);
    engine->root()->addChild(createEntity(
        engine.get(), groundMaterial.get(), "plane", Vector3(0.0f, 0.0f, 0.0f), Vector3(40.0f, 1.0f, 40.0f), false
    ));

    // Hero object: the glass-table model, rendered with Bayer8 opacity dithering.
    // Its (originally alpha-blended) StandardMaterials are switched to opaque-pass
    // screen-door dithering so the table both casts crisp soft shadows and reads as
    // partially transparent without any sorting artifacts.
    const auto glassTableResource = glassTableAsset->resource();
    if (glassTableResource) {
        auto* tableEntity = std::get<ContainerResource*>(*glassTableResource)->instantiateRenderEntity();
        tableEntity->setLocalScale(3.0f, 3.0f, 3.0f);
        tableEntity->setLocalPosition(-1.5f, 0.0f, 0.0f);
        engine->root()->addChild(tableEntity);

        // Convert every StandardMaterial on the table to dithered opacity.
        int ditheredCount = 0;
        for (auto* render : RenderComponent::instances()) {
            if (!render || !render->entity()) {
                continue;
            }
            auto* owner = render->entity();
            if (owner != tableEntity && !owner->isDescendantOf(tableEntity)) {
                continue;
            }
            render->setCastShadows(true);
            for (auto* mi : render->meshInstances()) {
                if (!mi) {
                    continue;
                }
                if (auto* stdMat = dynamic_cast<StandardMaterial*>(mi->material())) {
                    stdMat->setTransparent(false);       // dither runs in the opaque pass
                    stdMat->setOpacity(0.55f);           // screen-door coverage
                    stdMat->setOpacityDither(true);      // Bayer8 screen-door pattern
                    ++ditheredCount;
                }
            }
        }
        spdlog::info("Glass table loaded; {} material(s) switched to opacity dither", ditheredCount);
    } else {
        spdlog::error("Failed to load glass-table.glb");
    }

    // Transparency comparison spheres (alpha 0.55): left dithers, right blends.
    auto ditherMaterial = std::make_shared<StandardMaterial>();
    ditherMaterial->setDiffuse(Color(0.9f, 0.35f, 0.25f, 1.0f));
    ditherMaterial->setOpacity(0.55f);
    ditherMaterial->setOpacityDither(true);
    engine->root()->addChild(createEntity(
        engine.get(), ditherMaterial.get(), "sphere", Vector3(2.2f, 1.0f, 2.5f), Vector3(2.0f, 2.0f, 2.0f)
    ));

    auto blendMaterial = std::make_shared<StandardMaterial>();
    blendMaterial->setDiffuse(Color(0.25f, 0.45f, 0.9f, 1.0f));
    blendMaterial->setOpacity(0.55f);
    blendMaterial->setTransparent(true);
    engine->root()->addChild(createEntity(
        engine.get(), blendMaterial.get(), "sphere", Vector3(5.2f, 1.0f, 2.5f), Vector3(2.0f, 2.0f, 2.0f)
    ));

    // Dither matrix selection, applied to the dithered sphere so the patterns can be compared
    // side by side against the alpha-blended one.
    struct DitherEntry
    {
        DitherMode mode;
        const char* name;
    };
    constexpr DitherEntry ditherModes[] = {
        {DitherMode::DITHER_BAYER2, "Bayer2 (4 levels)"},
        {DitherMode::DITHER_BAYER4, "Bayer4 (16 levels)"},
        {DitherMode::DITHER_BAYER8, "Bayer8 (64 levels)"},
        {DitherMode::DITHER_BAYER16, "Bayer16 (256 levels)"},
    };
    size_t ditherModeIndex = 2;  // Bayer8, the default

    const auto applyDitherMode = [&]() {
        ditherMaterial->setOpacityDitherMode(ditherModes[ditherModeIndex].mode);
        spdlog::info("Dither matrix: {}", ditherModes[ditherModeIndex].name);
    };

    spdlog::info("PCSS demo: penumbraSize 0.04; 1 = PCF, 2 = PCSS, Space = auto-cycle, Esc = quit");
    spdlog::info("Dither: M cycles the matrix on the left sphere (Bayer 2/4/8/16)");

    bool running = true;
    bool usePcss = true;
    bool autoCycle = true;
    float cycleTimer = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    const auto applyShadowType = [&](const bool pcss) {
        usePcss = pcss;
        if (lightComponent) {
            lightComponent->setShadowType(pcss ? SHADOW_PCSS_32F : SHADOW_PCF3_32F);
        }
        spdlog::info("Shadow type: {}", pcss ? "PCSS_32F (contact hardening)" : "PCF3_32F");
    };
    applyShadowType(false);  // start with PCF

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_1) {
                autoCycle = false;
                applyShadowType(false);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_2) {
                autoCycle = false;
                applyShadowType(false);  // start with PCF
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_M) {
                ditherModeIndex = (ditherModeIndex + 1) % std::size(ditherModes);
                applyDitherMode();
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                autoCycle = true;
                cycleTimer = 0.0f;
                spdlog::info("Auto-cycle resumed");
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (autoCycle) {
            cycleTimer += static_cast<float>(dtSeconds);
            if (cycleTimer >= 3.5f) {
                cycleTimer = 0.0f;
                applyShadowType(!usePcss);
            }
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
