// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Local light PCSS demo (VisuTwin counterpart of upstream's
// contact-hardening-shadows test example): a Draco-compressed robot-arm.glb
// stands on a receiver floor lit by a spot light (warm, left) and an omni
// light (cool, right), both casting contact-hardening soft shadows (upstream
// shadowPCSS.js port). Auto-cycles PCF <-> PCSS on both lights every 3 s —
// with PCSS the arm's shadow sharpens at its base and softens with distance.
// The helipad env atlas provides the ambient/specular IBL.
// Keys: 1 = PCF, 2 = PCSS, Space = auto-cycle, Esc = quit.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>
#include <variant>

#include <QuartzCore/QuartzCore.hpp>

#include <core/shape/boundingBox.h>

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
#include "framework/handlers/containerResource.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Union AABB over every mesh instance owned by (or descended from) an entity —
// used to auto-scale/position the loaded GLB hero regardless of its authored size.
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
            if (!hasAny) { bbox = mi->aabb(); hasAny = true; }
            else { bbox.add(mi->aabb()); }
        }
    }
    return bbox;
}

Entity* createEntity(Engine* engine, Material* material, const char* type,
    const Vector3& position, const Vector3& scale, const bool castShadows)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
    if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
        render->setMaterial(material);
        render->setType(type);
        render->setCastShadows(castShadows);
        render->setReceiveShadows(true);
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
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        SDL_Quit();
    };

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Local PCSS Shadows", WINDOW_WIDTH, WINDOW_HEIGHT,
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
    scene->setAmbientLight(0.0f, 0.0f, 0.0f);

    // Helipad env atlas for image-based ambient/specular (upstream sets a low
    // skybox intensity so the local lights dominate the shading).
    auto helipadAsset = std::make_unique<Asset>(
        "helipad-env-atlas",
        AssetType::TEXTURE,
        rootPath + "/cubemaps/helipad-env-atlas.png",
        AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false}
    );
    if (const auto helipadResource = helipadAsset->resource()) {
        scene->setEnvAtlas(std::get<Texture*>(*helipadResource));
        scene->setSkyboxMip(1.0f);
        scene->setSkyboxIntensity(0.1f);
    } else {
        spdlog::warn("Failed to load helipad env atlas — continuing without IBL");
    }

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    camera->setLocalPosition(0.0f, 7.0f, 14.0f);
    camera->setLocalEulerAngles(-24.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    // Receiver-only floor (a large flat ground; a big caster would inflate the
    // shadow depth fits). Metallic + low gloss to match the upstream floor look.
    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setDiffuse(Color(0.5f, 0.5f, 0.52f, 1.0f));
    floorMaterial->setMetalness(0.7f);
    floorMaterial->setGlossInvert(false);
    floorMaterial->setGloss(0.25f);
    engine->root()->addChild(createEntity(
        engine.get(), floorMaterial.get(), "plane", Vector3(0.0f, 0.0f, 0.0f), Vector3(60.0f, 1.0f, 60.0f), false));

    // Hero shadow caster: Draco-compressed robot-arm.glb, auto-scaled from its
    // AABB to ~6 units tall and dropped so its base rests on the floor (y = 0).
    // Keep the Asset alive for the whole program — the instantiated render
    // entity references meshes/materials owned by the container resource.
    auto armAsset = std::make_unique<Asset>(
        "robot-arm",
        AssetType::CONTAINER,
        rootPath + "/models/robot-arm.glb"
    );
    Entity* robotArm = nullptr;
    if (const auto armResource = armAsset->resource();
        armResource && std::holds_alternative<ContainerResource*>(*armResource)) {
        if (auto* container = std::get<ContainerResource*>(*armResource)) {
            robotArm = container->instantiateRenderEntity();
        }
    }
    if (robotArm) {
        robotArm->setEngine(engine.get());
        engine->root()->addChild(robotArm);

        // Enable shadow casting/receiving on every render component in the tree.
        for (auto* render : RenderComponent::instances()) {
            if (!render || !render->entity()) {
                continue;
            }
            auto* owner = render->entity();
            if (owner == robotArm || owner->isDescendantOf(robotArm)) {
                render->setCastShadows(true);
                render->setReceiveShadows(true);
            }
        }

        const auto bbox = calcEntityAABB(robotArm);
        const auto& he = bbox.halfExtents();
        const auto& ct = bbox.center();
        const float maxExtent = std::max({he.getX(), he.getY(), he.getZ()}) * 2.0f;
        const float targetHeight = 6.0f;
        const float s = (maxExtent > 0.001f) ? (targetHeight / maxExtent) : 3.0f;
        // The AABB was measured with the model's own root scale applied, so compose
        // with it rather than replacing it (instantiateRenderEntity returns the glTF
        // root node itself for single-root scenes).
        const float s0 = robotArm->localScale().getX();
        robotArm->setLocalScale(s0 * s, s0 * s, s0 * s);
        // Recenter horizontally and lift so the scaled AABB minimum sits at y = 0.
        const float minY = (ct.getY() - he.getY()) * s;
        robotArm->setLocalPosition(-ct.getX() * s, -minY, -ct.getZ() * s);
        spdlog::info("robot-arm.glb: extent={:.2f}, scale={:.3f}", maxExtent, s);
    } else {
        spdlog::error("Failed to load/instantiate robot-arm.glb — is Draco enabled?");
    }

    // Spot light (warm) over the left of the arm.
    auto* spotLight = new Entity();
    spotLight->setEngine(engine.get());
    auto* spot = static_cast<LightComponent*>(spotLight->addComponent<LightComponent>());
    if (spot) {
        spot->setType(LightType::LIGHTTYPE_SPOT);
        spot->setColor(Color(1.0f, 0.95f, 0.85f, 1.0f));
        spot->setIntensity(2.6f);
        spot->setRange(30.0f);
        spot->setInnerConeAngle(35.0f);
        spot->setOuterConeAngle(55.0f);
        spot->setCastShadows(true);
        spot->setShadowResolution(1024);
        spot->setShadowBias(0.0005f);
        spot->setShadowNormalBias(0.02f);
        spot->setPenumbraSize(30.0f);  // local-light scale: search px on the shadow map
    }
    // Emission is the node's -Y axis (straight down). Hover above-left and
    // slightly behind the arm so its shadow stretches toward the camera.
    spotLight->setLocalPosition(-4.0f, 9.0f, -1.5f);
    engine->root()->addChild(spotLight);

    // Omni light (cool) to the right of the arm.
    auto* omniLight = new Entity();
    omniLight->setEngine(engine.get());
    auto* omni = static_cast<LightComponent*>(omniLight->addComponent<LightComponent>());
    if (omni) {
        omni->setType(LightType::LIGHTTYPE_OMNI);
        omni->setColor(Color(0.7f, 0.85f, 1.0f, 1.0f));
        omni->setIntensity(2.2f);
        omni->setRange(25.0f);
        omni->setCastShadows(true);
        omni->setShadowResolution(1024);
        omni->setShadowBias(0.0005f);
        omni->setPenumbraSize(30.0f);
    }
    omniLight->setLocalPosition(4.0f, 6.5f, 2.0f);
    engine->root()->addChild(omniLight);

    spdlog::info("Local PCSS: robot arm lit by spot (left, warm) + omni (right, cool), PCF <-> PCSS every 3 s");
    spdlog::info("Keys: 1 = PCF, 2 = PCSS, Space = auto-cycle, Esc = quit");

    const auto applyMode = [&](const bool pcss) {
        if (spot) spot->setShadowType(pcss ? SHADOW_PCSS_32F : SHADOW_PCF3_32F);
        if (omni) omni->setShadowType(pcss ? SHADOW_PCSS_32F : SHADOW_PCF3_32F);
        spdlog::info("Local shadow mode: {}", pcss ? "PCSS (contact-hardening)" : "PCF 3x3");
    };
    applyMode(false);

    bool running = true;
    bool usePcss = false;
    bool autoCycle = true;
    float cycleTimer = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_1) {
                autoCycle = false;
                usePcss = false;
                applyMode(false);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_2) {
                autoCycle = false;
                usePcss = true;
                applyMode(true);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                autoCycle = true;
                cycleTimer = 0.0f;
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (autoCycle) {
            cycleTimer += static_cast<float>(dtSeconds);
            if (cycleTimer >= 3.0f) {
                cycleTimer = 0.0f;
                usePcss = !usePcss;
                applyMode(usePcss);
            }
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
