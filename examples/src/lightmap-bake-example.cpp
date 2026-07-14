// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Lightmapper demo (port of PlayCanvas "lights-baked-a-o"): the house.glb scene
// — which ships with a generated UV1 channel unwrapped for lightmapping and
// stripped textures — is lit by a directional sun and baked (on the CPU) into
// per-mesh lightmaps with hard shadows + ambient occlusion. The whole house
// geometry is registered as ray occluders so submeshes shadow each other and
// AO forms in the crevices. The helipad env atlas provides the skybox / ambient.
// Each house mesh is masked out of realtime lighting (MASK_AFFECT_LIGHTMAPPED),
// so its whole look comes from the bake. Auto-toggles the lightmaps ON/OFF every
// 3 s to show the baked shadows + AO appear/disappear. Esc quits.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

#include <QuartzCore/QuartzCore.hpp>

#include "core/shape/boundingBox.h"
#include "framework/assets/asset.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "framework/lightmapper/lightmapper.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Helipad environment atlas: skybox + ambient (main source of ambient light).
const auto helipad = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{ .type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false }
);

// House scene: has a generated UV1 lightmap channel + stripped textures.
const auto house = std::make_unique<Asset>(
    "house",
    AssetType::CONTAINER,
    rootPath + "/models/house.glb"
);

// Union the AABB of every mesh instance under `entity` (for camera framing).
BoundingBox calcEntityAABB(Entity* entity)
{
    BoundingBox bbox;
    bbox.setCenter(0, 0, 0);
    bbox.setHalfExtents(0, 0, 0);
    if (!entity) {
        return bbox;
    }

    bool hasAny = false;
    for (auto* render : RenderComponent::instances()) {
        if (!render || !render->entity()) {
            continue;
        }
        auto* owner = render->entity();
        if (owner != entity && !owner->isDescendantOf(entity)) {
            continue;
        }
        for (auto* mi : render->meshInstances()) {
            if (!mi) {
                continue;
            }
            bbox.add(mi->aabb());
            hasAny = true;
        }
    }

    if (!hasAny) {
        bbox.setCenter(entity->position());
        bbox.setHalfExtents(1.0f, 1.0f, 1.0f);
    }
    return bbox;
}

// Gather every mesh instance under `entity` (all render components in the subtree).
std::vector<MeshInstance*> collectMeshInstances(Entity* entity)
{
    std::vector<MeshInstance*> out;
    for (auto* render : RenderComponent::instances()) {
        if (!render || !render->entity()) {
            continue;
        }
        auto* owner = render->entity();
        if (owner != entity && !owner->isDescendantOf(entity)) {
            continue;
        }
        for (auto* mi : render->meshInstances()) {
            if (mi && mi->mesh()) {
                out.push_back(mi);
            }
        }
    }
    return out;
}

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
        "VisuTwin Lightmap Bake", WINDOW_WIDTH, WINDOW_HEIGHT,
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

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setToneMapping(TONEMAP_ACES);

    // Skydome — the main source of ambient light (matches upstream).
    scene->setSkyboxMip(3);
    scene->setSkyboxIntensity(0.6f);
    scene->setAmbientLight(0.1f, 0.3f, 0.4f);
    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad env atlas");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    // Instantiate the house (unwrapped UV1 for lightmapping), scaled up like upstream.
    const auto houseResource = house->resource();
    if (!houseResource) {
        spdlog::error("Failed to load house model");
        shutdown();
        return -1;
    }
    auto* houseEntity = std::get<ContainerResource*>(*houseResource)->instantiateRenderEntity();
    houseEntity->setLocalScale(100.0f, 100.0f, 100.0f);
    engine->root()->addChild(houseEntity);

    // Every renderable mesh of the house.
    std::vector<MeshInstance*> houseMeshes = collectMeshInstances(houseEntity);
    spdlog::info("House has {} mesh instance(s)", houseMeshes.size());

    // ── Bake the house lightmaps ─────────────────────────────────────────────
    Lightmapper lightmapper(graphicsDevice.get());

    // Directional sun ~ matching upstream's directional light (euler -55, 0, -30,
    // color (0.7,0.7,0.5), intensity 1.6). DEVIATION: upstream additionally bakes
    // a yellow omni + red spot; omitted here for a clean single-light CPU bake.
    Lightmapper::Light sun;
    sun.type = LightType::LIGHTTYPE_DIRECTIONAL;
    sun.direction = Vector3(-0.41f, -0.71f, -0.57f).normalized();
    sun.color = Color(0.7f, 0.7f, 0.5f, 1.0f);
    sun.intensity = 1.6f;
    sun.castShadows = true;
    lightmapper.addLight(sun);

    // The whole house casts shadow/AO rays into the bake (targets included).
    for (auto* mi : houseMeshes) {
        if (auto* node = mi->node()) {
            lightmapper.addOccluder(*mi->mesh(), node->worldTransform());
        }
    }

    Lightmapper::Options bakeOptions;
    bakeOptions.lightmapSize = 256;   // per-mesh; kept modest since many submeshes bake
    bakeOptions.ambient = Color(0.05f, 0.09f, 0.12f, 1.0f);
    bakeOptions.skyColor = Color(0.12f, 0.2f, 0.3f, 1.0f);  // AO-modulated sky term
    bakeOptions.ambientOcclusion = true;
    bakeOptions.aoSamples = 16;
    bakeOptions.aoRadius = 60.0f;     // world units (house is scaled 100x)
    bakeOptions.dilatePixels = 6;

    // Bake each mesh, apply its lightmap, and mask it out of realtime lighting.
    // Track unique materials so the ON/OFF toggle can restore them.
    std::vector<std::pair<StandardMaterial*, std::shared_ptr<Texture>>> bakedMaterials;
    std::unordered_set<StandardMaterial*> seenMaterials;
    std::vector<std::shared_ptr<Texture>> keepAlive;  // hold every baked texture

    spdlog::info("Baking house lightmaps ({} meshes)...", houseMeshes.size());
    for (auto* mi : houseMeshes) {
        auto* node = mi->node();
        if (!node) {
            continue;
        }
        auto lightmap = lightmapper.bake(*mi->mesh(), node->worldTransform(), bakeOptions);
        if (!lightmap) {
            continue;
        }
        keepAlive.push_back(lightmap);
        mi->setMask(MASK_AFFECT_LIGHTMAPPED);

        if (auto* stdMat = dynamic_cast<StandardMaterial*>(mi->material())) {
            stdMat->setLightMap(lightmap.get());
            // DEVIATION/RISK: submeshes sharing one StandardMaterial but with
            // distinct UV1 layouts collide — last bake wins. Track the material
            // once so the toggle stays consistent.
            if (seenMaterials.insert(stdMat).second) {
                bakedMaterials.emplace_back(stdMat, std::move(lightmap));
            } else {
                bakedMaterials.back().second = keepAlive.back();
            }
        }
    }
    spdlog::info("Baked {} unique lightmapped material(s)", bakedMaterials.size());

    // Camera framing the house.
    const BoundingBox bbox = calcEntityAABB(houseEntity);
    const Vector3 center = bbox.center();
    const float radius = std::max(bbox.halfExtents().length(), 1.0f);
    const Vector3 offsetDir = Vector3(1.0f, 0.5f, 1.0f).normalized();
    const Vector3 camPos = center + offsetDir * (radius * 2.2f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    if (cameraComponent && cameraComponent->camera()) {
        cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        cameraComponent->camera()->setNearClip(1.0f);
        cameraComponent->camera()->setFarClip(std::max(radius * 8.0f, 200.0f));
    }
    camera->setLocalPosition(camPos.getX(), camPos.getY(), camPos.getZ());
    camera->setLocalEulerAngles(-19.5f, 45.0f, 0.0f);  // look back toward the house
    engine->root()->addChild(camera);

    spdlog::info("Lightmap bake demo. House lightmaps auto-toggle ON/OFF every 3 s. Esc quits.");

    bool running = true;
    bool lightmapOn = true;
    float toggleTimer = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        toggleTimer += static_cast<float>(dtSeconds);
        if (toggleTimer >= 3.0f) {
            toggleTimer = 0.0f;
            lightmapOn = !lightmapOn;
            for (auto& [mat, tex] : bakedMaterials) {
                mat->setLightMap(lightmapOn ? tex.get() : nullptr);
            }
            spdlog::info("House lightmaps: {}", lightmapOn ? "ON (baked shadows + AO)" : "OFF");
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
