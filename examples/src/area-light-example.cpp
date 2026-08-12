// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// LTC area-light demo (parity with PlayCanvas graphics/area-lights): a colored
// linearly-transformed-cosines area light illuminates the statue.glb hero standing on
// a seaside-rocks textured floor, lit by the helipad environment atlas. The light
// auto-cycles shape rect -> disk -> sphere and slowly tilts so the characteristic LTC
// stretched reflection sweeps across the glossy floor; Space pauses/resumes the tilt.
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
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "framework/assets/asset.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Environment atlas (image-based lighting + skydome), same asset the sibling examples use.
const auto helipad = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

// Statue hero model.
const auto statue = std::make_unique<Asset>(
    "statue",
    AssetType::CONTAINER,
    rootPath + "/models/statue.glb"
);

// Seaside-rocks floor textures (color / normal / gloss).
const auto floorColorTex = std::make_unique<Asset>(
    "floor-color", AssetType::TEXTURE, rootPath + "/textures/seaside-rocks01-color.jpg"
);
const auto floorNormalTex = std::make_unique<Asset>(
    "floor-normal", AssetType::TEXTURE, rootPath + "/textures/seaside-rocks01-normal.jpg"
);
const auto floorGlossTex = std::make_unique<Asset>(
    "floor-gloss", AssetType::TEXTURE, rootPath + "/textures/seaside-rocks01-gloss.jpg"
);

Texture* requireTexture(const std::unique_ptr<Asset>& asset, const char* label)
{
    const auto resource = asset->resource();
    if (!resource) {
        spdlog::error("Failed to load texture asset '{}'", label);
        return nullptr;
    }
    return std::get<Texture*>(*resource);
}

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
        "LTC Area Light", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    scene->setAmbientLight(0.06f, 0.06f, 0.07f);

    // Skydome + image-based lighting from the helipad environment atlas (darkened),
    // matching the PlayCanvas counterpart.
    scene->setSkyboxMip(1);
    scene->setSkyboxIntensity(0.4f);
    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad env atlas");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    // Elevated view looking down at the statue on the floor (PlayCanvas: pos (0,2.5,12), lookAt origin).
    camera->setLocalPosition(0.0f, 2.5f, 12.0f);
    camera->setLocalEulerAngles(-12.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    // Seaside-rocks textured ground plane (color + normal + gloss), metallic PBR so the
    // LTC area light produces a glossy stretched reflection across it. Textures tiled 7x7.
    Texture* floorColor = requireTexture(floorColorTex, "floor-color");
    Texture* floorNormal = requireTexture(floorNormalTex, "floor-normal");
    Texture* floorGloss = requireTexture(floorGlossTex, "floor-gloss");
    if (!floorColor || !floorNormal || !floorGloss) {
        shutdown();
        return -1;
    }

    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setName("seaside-rocks-floor");
    floorMaterial->setUseMetalness(true);
    floorMaterial->setMetalness(0.7f);
    floorMaterial->setGloss(0.8f);
    floorMaterial->setDiffuseMap(floorColor);
    floorMaterial->setNormalMap(floorNormal);
    floorMaterial->setGlossMap(floorGloss);
    floorMaterial->setDiffuseMapTiling(Vector2(7.0f, 7.0f));
    floorMaterial->setNormalMapTiling(Vector2(7.0f, 7.0f));
    engine->root()->addChild(createEntity(
        engine.get(), floorMaterial.get(), "plane", Vector3(0.0f, 0.0f, 0.0f), Vector3(20.0f, 20.0f, 20.0f)
    ));

    // Statue hero standing on the floor, lit by the area light (parity with orbit-example).
    const auto statueResource = statue->resource();
    if (!statueResource) {
        spdlog::error("Failed to load statue model");
        shutdown();
        return -1;
    }
    auto* statueEntity = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
    statueEntity->setLocalScale(0.4f, 0.4f, 0.4f); // match PlayCanvas area-lights framing
    statueEntity->setLocalPosition(0.0f, 0.0f, 0.0f);
    engine->root()->addChild(statueEntity);

    // The rect area light: a near-vertical warm billboard behind the scene facing the
    // camera. Entity -Y is the emission direction, X is the rect width axis.
    auto* areaLight = new Entity();
    areaLight->setEngine(engine.get());
    auto* lightComponent = static_cast<LightComponent*>(areaLight->addComponent<LightComponent>());
    if (lightComponent) {
        lightComponent->setType(LightType::LIGHTTYPE_AREA_RECT);
        lightComponent->setColor(Color(1.0f, 0.7f, 0.35f, 1.0f));
        lightComponent->setIntensity(1.5f);
        lightComponent->setRange(30.0f);
        lightComponent->setAreaWidth(9.6f);
        lightComponent->setAreaHeight(1.6f);
    }
    // Positioned above and in front of the statue so it lights the hero and casts the
    // stretched LTC reflection onto the floor toward the camera.
    areaLight->setLocalPosition(0.0f, 5.0f, 3.5f);
    engine->root()->addChild(areaLight);

    // Emissive slab so the panel itself is visible where the light comes from.
    auto panelMaterial = std::make_shared<StandardMaterial>();
    panelMaterial->setName("panel-emissive");
    panelMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
    panelMaterial->setEmissive(Color(1.0f, 0.7f, 0.35f, 1.0f));
    panelMaterial->setEmissiveIntensity(3.0f);
    auto* panel = createEntity(
        engine.get(), panelMaterial.get(), "box", Vector3(0.0f, 0.0f, 0.0f), Vector3(9.6f, 0.05f, 1.6f)
    );
    areaLight->addChild(panel);

    spdlog::info("LTC area light: 9.6m x 1.6m warm panel over floor strips roughness 0.05 -> 0.75");
    spdlog::info("Auto-cycles shape rect -> disk -> sphere every 4s. Keys: Space = pause/resume tilt, Esc = quit");

    // Shape cycle: rect -> disk -> sphere (disk inscribed in the panel quad;
    // sphere radius = max half-extent). The emissive slab is resized to match.
    const auto applyShape = [&](const int shapeIdx) {
        static const char* names[3] = {"RECT", "DISK", "SPHERE"};
        if (!lightComponent) return;
        lightComponent->setAreaShape(static_cast<AreaLightShape>(shapeIdx));
        if (shapeIdx == 2) {
            panel->setLocalScale(4.8f, 4.8f, 4.8f);  // sphere: uniform blob
        } else {
            panel->setLocalScale(9.6f, 0.05f, 1.6f);
        }
        spdlog::info("Area light shape: {}", names[shapeIdx]);
    };
    applyShape(0);

    bool running = true;
    bool animate = true;
    float animTime = 0.0f;
    float shapeTimer = 0.0f;
    int shapeIdx = 0;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    const auto poseLight = [&](const float t) {
        // Slow tilt oscillation around X: sweeps the reflection toward/away from camera.
        const float tilt = 82.0f + 8.0f * std::sin(t * 0.5f);
        areaLight->setLocalEulerAngles(tilt, 0.0f, 0.0f);
    };
    poseLight(0.0f);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                animate = !animate;
                spdlog::info("Tilt {}", animate ? "resumed" : "paused");
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (animate) {
            animTime += static_cast<float>(dtSeconds);
            poseLight(animTime);
        }

        shapeTimer += static_cast<float>(dtSeconds);
        if (shapeTimer >= 4.0f) {
            shapeTimer = 0.0f;
            shapeIdx = (shapeIdx + 1) % 3;
            applyShape(shapeIdx);
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
