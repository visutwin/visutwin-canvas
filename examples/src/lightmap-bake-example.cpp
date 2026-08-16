// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream's graphics/lights-baked-a-o: the house.glb scene
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
#include <chrono>
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
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "../cameraControls.h"
#include "framework/components/light/lightComponent.h"
#include "framework/lightmapper/gpuLightmapper.h"
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

// JS: app.scene.ambientLight = new Color(0.1, 0.3, 0.4) — also fed to the bake, since
// upstream's ambientBake samples this same ambient (or the cubemap) per texel.
const Color AMBIENT_LIGHT(0.1f, 0.3f, 0.4f, 1.0f);

// A light entity's euler angles to the world direction it emits along. Lights point
// down their node's -Y axis (the same convention the spot shadow camera corrects for).
Vector3 eulerToDirection(const float x, const float y, const float z)
{
    return (Quaternion::fromEulerAngles(x, y, z) * Vector3(0.0f, -1.0f, 0.0f)).normalized();
}

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
        "Lightmap Bake", WINDOW_WIDTH, WINDOW_HEIGHT,
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
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();

    // Skydome — the main source of ambient light (matches upstream).
    scene->setSkyboxMip(3);
    scene->setSkyboxIntensity(0.6f);
    scene->setAmbientLight(AMBIENT_LIGHT.r, AMBIENT_LIGHT.g, AMBIENT_LIGHT.b);
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

    const BoundingBox houseBounds = calcEntityAABB(houseEntity);

    // Every renderable mesh of the house.
    std::vector<MeshInstance*> houseMeshes = collectMeshInstances(houseEntity);
    spdlog::info("House has {} mesh instance(s)", houseMeshes.size());

    // ── Bake the house lightmaps ─────────────────────────────────────────────
    Lightmapper lightmapper(graphicsDevice.get());

    // JS: lightDirectional — euler (-55, 0, -30), color (0.7,0.7,0.5), intensity 1.6,
    //     bake with bakeNumSamples 15 over a bakeArea of 10 degrees (soft shadows).
    // Upstream creates these as real light entities with affectDynamic:false /
    // affectLightmapped:true / bake:true. The equivalent here is MASK_BAKE, which keeps
    // them out of realtime lighting while the GPU bake (which renders the mesh under
    // MASK_BAKE) still sees them. The CPU baker takes its own copies of the same values.
    const auto addBakeLight = [&](const char* name, const LightType type, const Color& color,
        const float intensity, const Vector3& position, const Vector3& eulerAngles,
        const float range) {
        auto* entity = new Entity();
        entity->setName(name);
        entity->setEngine(engine.get());
        engine->root()->addChild(entity);
        entity->setLocalPosition(position);
        entity->setLocalEulerAngles(eulerAngles.getX(), eulerAngles.getY(), eulerAngles.getZ());

        auto* light = static_cast<LightComponent*>(entity->addComponent<LightComponent>());
        light->setType(type);
        light->setColor(color);
        light->setIntensity(intensity);
        light->setCastShadows(true);
        // DEVIATION: upstream's shadowBias 0.2 is in its own units; here setShadowBias takes
        // the 0..1 authoring value and the pass applies bias * -1000 as a polygon offset, so
        // 0.2 would push casters ~4x further than the engine's usual 0.05 and erase the
        // small ones (the tree and the car lost their shadows entirely).
        light->setShadowBias(0.05f);
        light->setShadowNormalBias(0.05f);   // JS: normalOffsetBias 0.05
        light->setShadowResolution(2048);    // JS: shadowResolution 2048
        light->setMask(MASK_BAKE);
        if (range > 0.0f) {
            light->setRange(range);
        }
        return light;
    };

    auto* directionalLight = addBakeLight("Directional", LightType::LIGHTTYPE_DIRECTIONAL,
        Color(0.7f, 0.7f, 0.5f), 1.6f, Vector3(0.0f, 0.0f, 0.0f), Vector3(-55.0f, 0.0f, -30.0f), 0.0f);
    directionalLight->setShadowDistance(100.0f);
    auto* omniLight = addBakeLight("Omni", LightType::LIGHTTYPE_OMNI, Color(1.0f, 1.0f, 0.0f),
        0.9f, Vector3(-4.0f, 10.0f, 5.0f), Vector3(0.0f, 0.0f, 0.0f), 25.0f);
    auto* spotLight = addBakeLight("Spot", LightType::LIGHTTYPE_SPOT, Color(1.0f, 0.0f, 0.0f),
        2.5f, Vector3(-5.0f, 10.0f, -7.5f), Vector3(0.0f, 0.0f, 0.0f), 10.0f);

    auto gpuBakeStart = std::chrono::steady_clock::now();

    // GPU baker (upstream's UV-space render). Needs the lights to exist as real scene
    // LightComponents, since the bake evaluates the ordinary lit pipeline; the CPU baker
    // instead takes its own light descriptions below.
    GpuLightmapper gpuLightmapper(engine.get());

    // Upstream's HUD toggles; changing one re-bakes, exactly as upstream does
    // ("Bake when settings are changed only").
    bool directionalEnabled = true;   // data.directional.enabled
    bool otherLightsEnabled = true;   // data.other.enabled
    // DEVIATION: with ambient baking on, the GPU path's virtual lights carry the scene's
    // flat ambient colour rather than the env atlas radiance per direction (upstream's HUD
    // "cubemap" mode), so it tints the scene toward that colour. Off by default therefore —
    // key 3 turns it on to show the occlusion it adds. The CPU baker samples ambient the
    // same way but modulates it by ray-traced AO.
    bool ambientBakeEnabled = false;  // data.ambient.ambientBake
    bool hemisphereEnabled = true;    // data.ambient.hemisphere -> spherePart 0.4 vs 1
    bool filterEnabled = true;        // data.settings.lightmapFilterEnabled

    Lightmapper::Light sun;
    sun.type = LightType::LIGHTTYPE_DIRECTIONAL;
    sun.direction = eulerToDirection(-55.0f, 0.0f, -30.0f);
    sun.color = Color(0.7f, 0.7f, 0.5f, 1.0f);
    sun.intensity = 1.6f;
    sun.castShadows = true;
    sun.bakeNumSamples = 15;
    sun.bakeArea = 10.0f;

    // JS: lightOmni — yellow, position (-4, 10, 5), range 25, intensity 0.9, bake: true
    Lightmapper::Light omni;
    omni.type = LightType::LIGHTTYPE_OMNI;
    omni.position = Vector3(-4.0f, 10.0f, 5.0f);
    omni.color = Color(1.0f, 1.0f, 0.0f, 1.0f);
    omni.intensity = 0.9f;
    omni.range = 25.0f;
    omni.castShadows = true;

    // JS: lightSpot — red, position (-5, 10, -7.5), range 10, intensity 2.5, bake: true.
    // Upstream leaves the spot at its default rotation, which points straight down.
    Lightmapper::Light spot;
    spot.type = LightType::LIGHTTYPE_SPOT;
    spot.position = Vector3(-5.0f, 10.0f, -7.5f);
    spot.direction = Vector3(0.0f, -1.0f, 0.0f);
    spot.color = Color(1.0f, 0.0f, 0.0f, 1.0f);
    spot.intensity = 2.5f;
    spot.range = 10.0f;
    spot.castShadows = true;

    // The whole house casts shadow/AO rays into the bake (targets included).
    for (auto* mi : houseMeshes) {
        if (auto* node = mi->node()) {
            lightmapper.addOccluder(*mi->mesh(), node->worldTransform());
        }
    }

    // JS: app.scene.lightmapMode = BAKE_COLOR; lightmapMaxResolution = 1024;
    //     lightmapSizeMultiplier = 512; plus the HUD defaults for ambient + filter.
    Lightmapper::Options bakeOptions;
    bakeOptions.sizeMultiplier = 512.0f;
    bakeOptions.maxResolution = 1024;
    bakeOptions.ambient = AMBIENT_LIGHT;               // scene ambient, baked in
    bakeOptions.skyColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
    bakeOptions.ambientBake = true;
    bakeOptions.ambientBakeNumSamples = 20;
    bakeOptions.ambientBakeSpherePart = 0.4f;          // HUD "hemisphere" on
    bakeOptions.ambientBakeOcclusionContrast = -0.6f;
    bakeOptions.ambientBakeOcclusionBrightness = -0.5f;
    bakeOptions.aoRadius = 40.0f;                      // occlusion ray reach, world units
    bakeOptions.filterEnabled = true;
    bakeOptions.filterRange = 10.0f;
    bakeOptions.filterSmoothness = 0.2f;
    bakeOptions.dilatePixels = 6;

    // Bake each mesh, apply its lightmap, and mask it out of realtime lighting.
    // Track unique materials so the ON/OFF toggle can restore them.
    std::vector<std::pair<StandardMaterial*, std::shared_ptr<Texture>>> bakedMaterials;
    std::unordered_set<StandardMaterial*> seenMaterials;
    std::vector<std::shared_ptr<Texture>> keepAlive;  // hold every baked texture

    // Re-bake with the current flags. Upstream re-bakes whenever a HUD setting
    // changes and reports the duration in its "Bake stats" panel.
    const auto rebake = [&]() {
        lightmapper.clear();
        if (directionalEnabled) {
            lightmapper.addLight(sun);
        }
        if (otherLightsEnabled) {
            lightmapper.addLight(omni);
            lightmapper.addLight(spot);
        }
        for (auto* mi : houseMeshes) {
            if (auto* node = mi->node()) {
                lightmapper.addOccluder(*mi->mesh(), node->worldTransform());
            }
        }

        bakeOptions.ambientBake = ambientBakeEnabled;
        bakeOptions.ambientBakeSpherePart = hemisphereEnabled ? 0.4f : 1.0f;
        bakeOptions.filterEnabled = filterEnabled;

        bakedMaterials.clear();
        seenMaterials.clear();
        keepAlive.clear();

        const auto bakeStart = std::chrono::steady_clock::now();
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
        const auto bakeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - bakeStart).count();
        // JS: data.stats.duration
        spdlog::info("Bake duration: {} ms ({} lightmapped material(s))", bakeMs, bakedMaterials.size());
    };

    // The GPU bake is upstream's own mechanism and costs a single frame, so it is what
    // the example starts with; the CPU ray-traced bake (rebake(), key C) stays available
    // as the higher-quality reference — true AO and soft shadows for seconds of work.
    const auto gpuRebake = [&]() {
        directionalLight->setEnabled(directionalEnabled);
        omniLight->setEnabled(otherLightsEnabled);
        spotLight->setEnabled(otherLightsEnabled);

        GpuLightmapper::Options gpuOptions;
        gpuOptions.sizeMultiplier = 512.0f;    // JS: app.scene.lightmapSizeMultiplier
        gpuOptions.maxResolution = 1024;       // JS: app.scene.lightmapMaxResolution       // JS: app.scene.lightmapMaxResolution
        // JS: ambientBake with ambientBakeNumSamples 20 over spherePart 0.4 — here those
        // become virtual directional lights accumulated into the lightmap.
        // JS: the directional light bakes with bakeNumSamples 15 over a bakeArea of 10
        // degrees — soft-edged shadows rather than one hard shadow map.
        gpuOptions.directionalBakeNumSamples = 15;
        gpuOptions.directionalBakeArea = 10.0f;
        gpuOptions.ambientBake = ambientBakeEnabled;
        gpuOptions.ambientBakeNumSamples = 20;
        gpuOptions.ambientBakeSpherePart = hemisphereEnabled ? 0.4f : 1.0f;
        gpuOptions.bakeCameraTarget = houseBounds.center();
        gpuOptions.bakeCameraDistance = std::max(houseBounds.halfExtents().length() * 2.0f, 50.0f);
        gpuBakeStart = std::chrono::steady_clock::now();
        gpuLightmapper.bake(houseMeshes, gpuOptions);
    };

    spdlog::info("Baking house lightmaps on the GPU ({} meshes)...", houseMeshes.size());
    gpuRebake();

    // JS: camera clearColor (0.4, 0.45, 0.5), farClip 100, nearClip 1, position (40, 20, 40),
    //     orbiting the house. DEVIATION: upstream's orbit-camera script becomes lookAt here.
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    if (cameraComponent && cameraComponent->camera()) {
        cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        cameraComponent->camera()->setNearClip(1.0f);
        cameraComponent->camera()->setFarClip(100.0f);
    }
    engine->root()->addChild(camera);
    camera->setLocalPosition(40.0f, 20.0f, 40.0f);

    // JS: camera.script.create('orbitCamera', { inertiaFactor: 0.2, focusEntity: house,
    //     distanceMax: 60 }) + orbitCameraInputMouse/Touch — drag to orbit, wheel to zoom.
    camera->lookAt(houseBounds.center());
    auto* cameraScript = static_cast<ScriptComponent*>(camera->addComponent<ScriptComponent>());
    auto* cameraControls = cameraScript ? cameraScript->create<CameraControls>() : nullptr;
    if (cameraControls) {
        cameraControls->setFocusPoint(houseBounds.center());
        cameraControls->setZoomRange(Vector2(5.0f, 60.0f));   // JS: distanceMax 60
    }

    // Upstream's HUD panels become keys; each one re-bakes like the HUD does.
    spdlog::info("Keys: 1 directional  2 other lights (GPU re-bake)  |  "
                 "3 ambient bake  4 hemisphere  5 lightmap filter (CPU re-bake)  |  "
                 "G GPU bake  C CPU ray-traced bake  L show/hide  |  ESC quits");

    bool running = true;
    bool lightmapOn = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                bool bakeSettingChanged = true;
                switch (event.key.key) {
                case SDLK_ESCAPE:
                    running = false;
                    bakeSettingChanged = false;
                    break;
                case SDLK_1:
                    directionalEnabled = !directionalEnabled;
                    spdlog::info("Directional light: {}", directionalEnabled ? "on" : "off");
                    break;
                case SDLK_2:
                    otherLightsEnabled = !otherLightsEnabled;
                    spdlog::info("Other lights (omni + spot): {}", otherLightsEnabled ? "on" : "off");
                    break;
                case SDLK_3:
                    ambientBakeEnabled = !ambientBakeEnabled;
                    spdlog::info("Ambient bake: {}", ambientBakeEnabled ? "on" : "off");
                    break;
                case SDLK_4:
                    hemisphereEnabled = !hemisphereEnabled;
                    spdlog::info("Ambient hemisphere: {}", hemisphereEnabled ? "on (0.4)" : "off (full sphere)");
                    break;
                case SDLK_5:
                    filterEnabled = !filterEnabled;
                    spdlog::info("Lightmap filter: {}", filterEnabled ? "on" : "off");
                    break;
                case SDLK_G:
                    gpuRebake();
                    bakeSettingChanged = false;
                    break;
                case SDLK_C:
                    spdlog::info("CPU ray-traced re-bake...");
                    rebake();
                    lightmapOn = true;
                    bakeSettingChanged = false;
                    break;
                case SDLK_L:
                    lightmapOn = !lightmapOn;
                    for (auto& [mat, tex] : bakedMaterials) {
                        mat->setLightMap(lightmapOn ? tex.get() : nullptr);
                    }
                    spdlog::info("House lightmaps: {}", lightmapOn ? "ON (baked shadows + AO)" : "OFF");
                    bakeSettingChanged = false;
                    break;
                default:
                    bakeSettingChanged = false;
                    break;
                }
                if (bakeSettingChanged) {
                    // Lights affect both bakers; the CPU-only quality knobs (3/4/5) still
                    // need the ray-traced path, so those re-bake on the CPU.
                    if (event.key.key == SDLK_1 || event.key.key == SDLK_2 ||
                        event.key.key == SDLK_3 || event.key.key == SDLK_4) {
                        gpuRebake();
                    } else {
                        rebake();
                        lightmapOn = true;
                    }
                }
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        engine->update(static_cast<float>(dtSeconds));
        engine->render();

        // Collect a GPU bake once its frame has rendered (same pattern as ReflectionProbe).
        if (gpuLightmapper.baking() && gpuLightmapper.update()) {
            const auto gpuMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - gpuBakeStart).count();
            spdlog::info("GPU bake duration: {} ms", gpuMs);
            lightmapOn = true;
        }
    }

    shutdown();
    return 0;
}
