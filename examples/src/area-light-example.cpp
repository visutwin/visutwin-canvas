// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// LTC area-light demo (parity with PlayCanvas graphics/area-lights): three animated
// area lights — a white rect, a yellow sphere and a large blue "sky" disk — illuminate
// the statue.glb hero standing on a seaside-rocks textured floor, lit by the helipad
// environment atlas. Each light carries an emissive primitive matching its shape.
//
// DEVIATION: upstream builds these as spot/omni/directional lights with an area SHAPE
// (cone clipping, shadows, no distance falloff on the directional). This engine has a
// single positional LIGHTTYPE_AREA_RECT type (two-sided, range-windowed, no shadows),
// so all three are area lights; the directional disk is emulated by placing a large
// disk far away along the animated direction (same angular size as upstream's).
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

float lerp(const float a, const float b, const float t)
{
    return a + (b - a) * t;
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
        "Area Lights", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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

    // Seaside-rocks textured ground plane (color + normal + gloss), metallic PBR so the
    // LTC area lights produce glossy stretched reflections across it. Textures tiled 7x7.
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
    floorMaterial->setMetalnessMapTiling(Vector2(7.0f, 7.0f));

    auto* floor = new Entity();
    floor->setEngine(engine.get());
    floor->setLocalScale(20.0f, 20.0f, 20.0f);
    if (auto* render = static_cast<RenderComponent*>(floor->addComponent<RenderComponent>())) {
        render->setMaterial(floorMaterial.get());
        render->setType("plane");
    }
    engine->root()->addChild(floor);

    // Statue hero standing on the floor (PlayCanvas scale 0.4).
    const auto statueResource = statue->resource();
    if (!statueResource) {
        spdlog::error("Failed to load statue model");
        shutdown();
        return -1;
    }
    auto* statueEntity = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
    statueEntity->setLocalScale(0.4f, 0.4f, 0.4f);
    engine->root()->addChild(statueEntity);

    // Camera matching PlayCanvas: pos (0, 2.5, 12), lookAt origin, fov 60, gray clear.
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    if (cameraComp) {
        cameraComp->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
        cameraComp->camera()->setFov(60.0f);
        cameraComp->camera()->setFarClip(100000.0f);
    }
    camera->setLocalPosition(0.0f, 2.5f, 12.0f);
    // lookAt(0,0,0) from (0,2.5,12): pitch = -atan2(2.5, 12).
    camera->setLocalEulerAngles(-11.77f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    // Helper mirroring upstream createAreaLight: a parent entity carrying the area
    // light plus an emissive primitive matching the light-source shape.
    const auto createAreaLight = [&](const AreaLightShape shape, const Vector3& position,
        const float size, const Color& color, const float intensity, const float range,
        std::shared_ptr<StandardMaterial>& outMaterial) -> Entity* {
        auto* lightParent = new Entity();
        lightParent->setEngine(engine.get());
        lightParent->setLocalPosition(position.getX(), position.getY(), position.getZ());
        engine->root()->addChild(lightParent);

        auto* lightEntity = new Entity();
        lightEntity->setEngine(engine.get());
        if (auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_AREA_RECT);
            light->setAreaShape(shape);
            light->setColor(color);
            light->setIntensity(intensity);
            light->setRange(range);
            light->setAreaWidth(size);
            light->setAreaHeight(size);
        }
        lightParent->addChild(lightEntity);

        // Emissive primitive that is the light source color (upstream: emissive
        // material with lighting off; plane for rect, sphere, flattened cone for disk).
        auto brightMaterial = std::make_shared<StandardMaterial>();
        // The unlit path outputs base color (not emissive), so the source color
        // rides in diffuse.
        brightMaterial->setDiffuse(color);
        brightMaterial->setEmissive(color);
        brightMaterial->setUseLighting(false);
        brightMaterial->setCullMode(
            shape == AreaLightShape::LIGHTSHAPE_RECT ? CullMode::CULLFACE_NONE : CullMode::CULLFACE_BACK
        );
        outMaterial = brightMaterial;

        auto* brightShape = new Entity();
        brightShape->setEngine(engine.get());
        if (auto* render = static_cast<RenderComponent*>(brightShape->addComponent<RenderComponent>())) {
            render->setMaterial(brightMaterial.get());
            render->setType(
                shape == AreaLightShape::LIGHTSHAPE_SPHERE ? "sphere" :
                shape == AreaLightShape::LIGHTSHAPE_DISK ? "cone" : "plane"
            );
        }
        brightShape->setLocalScale(
            size, shape == AreaLightShape::LIGHTSHAPE_DISK ? 0.001f : size, size
        );
        lightParent->addChild(brightShape);

        return lightParent;
    };

    // Emulated-directional disk: upstream places a disk of angular diameter
    // scale/range = 0.2 rad at distance far=5000; we reproduce the same angular
    // size at a nearer distance the positional area light can handle.
    constexpr float kDiskDistance = 100.0f;
    constexpr float kDiskSize = 0.2f * kDiskDistance;

    // Three lights matching upstream: white rect, yellow sphere, blue "sky" disk.
    std::shared_ptr<StandardMaterial> rectMat, sphereMat, diskMat;
    Entity* light1 = createAreaLight(
        AreaLightShape::LIGHTSHAPE_RECT, Vector3(-3.0f, 4.0f, 0.0f), 4.0f,
        Color(1.0f, 1.0f, 1.0f, 1.0f), 2.0f, 10.0f, rectMat
    );
    Entity* light2 = createAreaLight(
        AreaLightShape::LIGHTSHAPE_SPHERE, Vector3(5.0f, 2.0f, -2.0f), 2.0f,
        Color(1.0f, 1.0f, 0.0f, 1.0f), 2.0f, 10.0f, sphereMat
    );
    Entity* light3 = createAreaLight(
        AreaLightShape::LIGHTSHAPE_DISK, Vector3(0.0f, 0.0f, 0.0f), kDiskSize,
        Color(0.7f, 0.7f, 1.0f, 1.0f), 10.0f, 1000.0f, diskMat
    );

    spdlog::info("Area lights: white rect + yellow sphere + blue sky disk over statue");
    spdlog::info("Keys: Space = pause/resume animation, Esc = quit");

    bool running = true;
    bool animate = true;
    float time = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    // Per-frame animation mirroring the upstream update callback.
    const auto animateLights = [&](const float t) {
        const float factor1 = (std::sin(t) + 1.0f) * 0.5f;
        const float factor2 = (std::sin(t * 0.6f) + 1.0f) * 0.5f;
        const float factor3 = (std::sin(t * 0.4f) + 1.0f) * 0.5f;

        light1->setLocalEulerAngles(lerp(-90.0f, 110.0f, factor1), 0.0f, 90.0f);
        light1->setLocalPosition(-4.0f, lerp(2.0f, 4.0f, factor3), lerp(-2.0f, 2.0f, factor2));

        light2->setLocalPosition(5.0f, lerp(1.0f, 3.0f, factor1), lerp(-2.0f, 2.0f, factor2));

        light3->setLocalEulerAngles(
            lerp(230.0f, 310.0f, factor2), lerp(-30.0f, 0.0f, factor3), 90.0f
        );
        // Upstream: position = camera + lightY * far (the disk hangs in the sky
        // along its emission axis). Matrix4::getElement takes (col, row); Y axis = col 1.
        const auto& wt = light3->worldTransform();
        const Vector3 dir(wt.getElement(1, 0), wt.getElement(1, 1), wt.getElement(1, 2));
        const Vector3 camPos = camera->position();
        light3->setLocalPosition(
            camPos.getX() + dir.getX() * kDiskDistance,
            camPos.getY() + dir.getY() * kDiskDistance,
            camPos.getZ() + dir.getZ() * kDiskDistance
        );
    };
    animateLights(0.0f);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                animate = !animate;
                spdlog::info("Animation {}", animate ? "resumed" : "paused");
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (animate) {
            time += static_cast<float>(dtSeconds);
            animateLights(time);
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
