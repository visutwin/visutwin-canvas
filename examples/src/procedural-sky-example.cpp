// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of the upstream "procedural-sky" example.
//
// A laboratory bedded into dry-sand dunes, lit by a single sun that sweeps across a
// time-of-day cycle. The hour drives the sun's elevation and azimuth through smoothstep
// keyframe curves; the sun's elevation in turn drives the sky luminance, the bloom
// intensity, and the torches inside the laboratory, which glow only between sunset and
// sunrise. Shadows are PCSS over four cascades and re-render every frame, since the sun
// never stops moving.
//
// DEVIATION — the sky model. Upstream attaches its own `ProceduralSky` ESM script, which
// renders a Preetham analytic daylight sky (a port of the three.js `Sky` shader) into an
// equirect texture, bakes image-based lighting from it, and adds a night sky with stars,
// a moon disk and a twilight band. This port drives the engine's built-in Nishita
// single-scattering atmosphere from the same sun direction instead. The scene, the
// lighting, the framing, the effects and the time-of-day behaviour all match; the sky
// itself is a different scattering model and has no night phase, so the cycle here skips
// the night exactly as upstream's does (20:00 wraps back to 05:00).
//
// @credit Laboratory by Sketchfab, CC BY 4.0
// @credit FREE - Dry Sand Terrain by josevega, Sketchfab, CC BY 4.0
//
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#ifdef VISUTWIN_HAS_METAL
#include <QuartzCore/QuartzCore.hpp>
#endif

#include "../cameraControls.h"
#include "core/math/curve.h"
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

constexpr int WINDOW_WIDTH = 1024;
constexpr int WINDOW_HEIGHT = 768;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Upstream's initial control values.
constexpr float INITIAL_HOUR = 9.0f;
constexpr float TIME_SPEED = 1.0f;      // hours per second
constexpr float SKY_EXPOSURE = 1.8f;
constexpr float TORCH_INTENSITY = 60.0f;

const auto laboratoryAsset = std::make_unique<Asset>(
    "laboratory", AssetType::CONTAINER, rootPath + "/models/laboratory.glb");
const auto terrainAsset = std::make_unique<Asset>(
    "terrain", AssetType::CONTAINER, rootPath + "/models/dry-sand-terrain.glb");

// GPU-side atmosphere uniform block. MUST be a byte-exact mirror of
// UniformBinder::AtmosphereUniforms (6 x float4 = 96 bytes) — a mis-sized or re-ordered
// field silently corrupts the shader read and produces a black/broken sky.
struct alignas(16) AtmosphereData
{
    // The planet centre is CAMERA-LOCAL: the shader builds its ray origin as
    // -planetCenter / planetRadius, so a viewer standing on the surface must put the
    // centre one planet radius below itself. Leaving this at the origin places the camera
    // at the planet's core, every view ray starts underground, and the sky renders black —
    // which is exactly what this example did before.
    float planetCenterAndRadius[4]           = {0.0f, -6371000.0f, 0.0f, 6371000.0f};
    float atmosphereRadiusAndSunIntensity[4] = {6471000.0f, 22.0f, 0.9998f, 0.0f};
    float rayleighCoeffAndScaleHeight[4]     = {5.5e-6f, 13.0e-6f, 22.4e-6f, 8500.0f};
    float mieCoeffAndScaleHeight[4]          = {21.0e-6f, 1200.0f, 0.758f, 0.0f};
    float sunDirection[4]                    = {0.0f, 1.0f, 0.0f, 0.0f};
    float cameraAltitudeAndParams[4]         = {0.0f, 32.0f, 8.0f, 0.0f};
};
static_assert(sizeof(AtmosphereData) == 96, "AtmosphereData must be 96 bytes (6 x float4)");

// Accumulated world-space bounds of every mesh instance under a node.
BoundingBox entityBounds(GraphNode* root)
{
    BoundingBox bounds;
    bool first = true;
    for (auto* render : RenderComponent::instances()) {
        auto* owner = render ? render->entity() : nullptr;
        if (!owner || (owner != root && !owner->isDescendantOf(root))) {
            continue;
        }
        for (auto* meshInstance : render->meshInstances()) {
            if (!meshInstance) continue;
            if (first) {
                bounds = meshInstance->aabb();
                first = false;
            } else {
                bounds.add(meshInstance->aabb());
            }
        }
    }
    return bounds;
}

Curve makeCurve(const std::vector<float>& keys)
{
    Curve curve(keys);
    curve.type = CURVE_SMOOTHSTEP;
    return curve;
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

#ifdef VISUTWIN_HAS_METAL
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
#endif
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Procedural Sky", WINDOW_WIDTH, WINDOW_HEIGHT,
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

    auto scene = engine->scene();
    scene->setSkyType(SKYTYPE_ATMOSPHERE);
    scene->setAtmosphereEnabled(true);
    scene->setExposure(SKY_EXPOSURE);

    // ---------------------------------------------------------------------
    // Laboratory
    // ---------------------------------------------------------------------
    const auto labResource = laboratoryAsset->resource();
    if (!labResource) {
        spdlog::error("Failed to load models/laboratory.glb");
        shutdown();
        return -1;
    }
    auto* labEntity = std::get<ContainerResource*>(*labResource)->instantiateRenderEntity();
    labEntity->setEngine(engine.get());
    labEntity->setLocalScale(100.0f, 100.0f, 100.0f);
    engine->root()->addChild(labEntity);

    // Materials use SSAO only — drop the baked AO map, and keep everything opaque.
    for (auto* render : RenderComponent::instances()) {
        auto* owner = render ? render->entity() : nullptr;
        if (!owner || (owner != labEntity && !owner->isDescendantOf(labEntity))) {
            continue;
        }
        render->setCastShadows(true);
        render->setReceiveShadows(true);
        for (auto* meshInstance : render->meshInstances()) {
            if (auto* material = dynamic_cast<StandardMaterial*>(
                    meshInstance ? meshInstance->material() : nullptr)) {
                material->setAoMap(nullptr);
                material->setTransparent(false);
            }
        }
    }

    // Torches: every node named 'Fackel*' gets a warm omni light whose intensity is
    // driven by the day/night cycle, so they only glow between sunset and sunrise.
    std::vector<LightComponent*> torchLights;
    for (auto* torch : labEntity->find([](GraphNode* node) {
             return node && node->name().find("Fackel") != std::string::npos;
         })) {
        // The mesh sits on a child node (the glTF splits node/primitive), so search the
        // subtree rather than the node itself — upstream's findComponent does the same.
        auto* torchEntity = dynamic_cast<Entity*>(torch);
        const auto renders = torchEntity ? torchEntity->findComponents<RenderComponent>()
                                         : std::vector<RenderComponent*>{};
        RenderComponent* render = nullptr;
        for (auto* candidate : renders) {
            if (candidate && !candidate->meshInstances().empty()) {
                render = candidate;
                break;
            }
        }
        if (!render) {
            continue;
        }
        auto* light = new Entity();
        light->setEngine(engine.get());
        if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lc->setType(LightType::LIGHTTYPE_OMNI);
            lc->setColor(Color(1.0f, 0.55f, 0.2f, 1.0f));
            lc->setIntensity(0.0f);
            lc->setRange(480.0f);
            lc->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
            torchLights.push_back(lc);
        }
        // Place at the flame's world-space position.
        const auto centre = render->meshInstances()[0]->aabb().center();
        light->setLocalPosition(centre.getX(), centre.getY(), centre.getZ());
        engine->root()->addChild(light);
    }
    spdlog::info("Torch lights found: {}", torchLights.size());

    // ---------------------------------------------------------------------
    // Terrain — scaled to span the far clip and bedded under the laboratory
    // ---------------------------------------------------------------------
    const auto terrainResource = terrainAsset->resource();
    if (!terrainResource) {
        spdlog::error("Failed to load models/dry-sand-terrain.glb");
        shutdown();
        return -1;
    }
    auto* terrain = std::get<ContainerResource*>(*terrainResource)->instantiateRenderEntity();
    terrain->setEngine(engine.get());
    engine->root()->addChild(terrain);

    const BoundingBox terrainAabb = entityBounds(terrain);
    constexpr float groundLevel = -40.0f;
    const float terrainScale = 3000.0f /
        (2.0f * std::max(terrainAabb.halfExtents().getX(), terrainAabb.halfExtents().getZ()));
    terrain->setLocalScale(terrainScale, terrainScale, terrainScale);

    const Vector3 tc = terrainAabb.center();
    const float terrainTop = (tc.getY() + terrainAabb.halfExtents().getY()) * terrainScale;
    terrain->setLocalPosition(
        -tc.getX() * terrainScale - 71.6f,
        groundLevel - terrainTop + 267.1f,
        -tc.getZ() * terrainScale + 395.8f);

    // Dim the bright sand by half so it balances against the darker building.
    std::vector<Material*> dimmed;
    for (auto* render : RenderComponent::instances()) {
        auto* owner = render ? render->entity() : nullptr;
        if (!owner || (owner != terrain && !owner->isDescendantOf(terrain))) {
            continue;
        }
        render->setCastShadows(true);
        render->setReceiveShadows(true);
        for (auto* meshInstance : render->meshInstances()) {
            auto* material = dynamic_cast<StandardMaterial*>(
                meshInstance ? meshInstance->material() : nullptr);
            if (!material || std::find(dimmed.begin(), dimmed.end(), material) != dimmed.end()) {
                continue;
            }
            dimmed.push_back(material);
            const Color& d = material->diffuse();
            material->setDiffuse(Color(d.r * 0.5f, d.g * 0.5f, d.b * 0.5f, d.a));
        }
    }

    // ---------------------------------------------------------------------
    // Sun — PCSS soft shadows over four cascades, re-rendered every frame
    // ---------------------------------------------------------------------
    auto* sunEntity = new Entity();
    sunEntity->setEngine(engine.get());
    auto* sun = static_cast<LightComponent*>(sunEntity->addComponent<LightComponent>());
    if (sun) {
        sun->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        sun->setCastShadows(true);
        sun->setIntensity(6.0f);
        sun->setShadowType(ShadowType::SHADOW_PCSS_32F);
        sun->setPenumbraSize(0.03f);
        sun->setPenumbraFalloff(2.1f);
        sun->setShadowResolution(2048);
        sun->setNumCascades(4);
        sun->setCascadeDistribution(0.35f);
        sun->setShadowBias(0.18f);
        sun->setShadowNormalBias(0.82f);
        sun->setShadowDistance(2400.0f);
    }
    engine->root()->addChild(sunEntity);

    // ---------------------------------------------------------------------
    // Camera — wide angle so more of the sky is visible
    // ---------------------------------------------------------------------
    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    cameraEntity->addComponent<ScriptComponent>();
    cameraEntity->setLocalPosition(240.0f, 85.0f, 240.0f);
    engine->root()->addChild(cameraEntity);

    if (cameraComponent) {
        if (auto* camera = cameraComponent->camera()) {
            camera->setFov(80.0f);
            camera->setFarClip(3000.0f);
        }
        cameraComponent->setToneMapping(TONEMAP_NEUTRAL);

        // SSAO applied to the ambient lighting rather than as a post-process.
        auto ssao = cameraComponent->ssao();
        ssao.enabled = true;
        ssao.type = "lighting";
        ssao.blurEnabled = true;
        ssao.intensity = 0.4f;
        ssao.power = 6.0f;
        ssao.radius = 30.0f;
        ssao.samples = 12;
        ssao.minAngle = 10.0f;
        cameraComponent->setSsao(ssao);

        auto rendering = cameraComponent->rendering();
        rendering.toneMapping = TONEMAP_NEUTRAL;
        rendering.bloomIntensity = 0.0f;   // driven by the sun elevation below
        rendering.bloomBlurLevel = 16;
        cameraComponent->setRendering(rendering);
    }

    const Vector3 focusPoint(0.0f, 25.0f, 0.0f);
    auto* cameraControls = cameraEntity->script()->create<CameraControls>();
    cameraControls->setFocusPoint(focusPoint);
    cameraControls->setEnableFly(false);
    cameraControls->setZoomRange(Vector2(1.0f, 500.0f));
    cameraControls->storeResetState();

    // ---------------------------------------------------------------------
    // Time-of-day curves (upstream's editable keyframes, smoothstepped)
    // ---------------------------------------------------------------------
    Curve elevationCurve = makeCurve({0.0f, -60.0f, 6.0f, 0.0f, 12.0f, 60.0f, 18.0f, 0.0f, 24.0f, -90.0f});
    Curve luminanceCurve = makeCurve({0.0f, 2.0f, 35.0f, 0.4f, 90.0f, 0.3f});
    Curve bloomCurve     = makeCurve({0.0f, 0.005f, 5.0f, 0.001f, 8.0f, 0.001f, 90.0f, 0.002f});

    float hour = INITIAL_HOUR;
    bool animate = true;
    float lastBloom = -1.0f;

    spdlog::info("Procedural sky: Space pauses the day cycle, R resets the camera, Esc quits.");

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
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                animate = !animate;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            } else if (event.type == SDL_EVENT_PINCH_UPDATE && cameraControls) {
                cameraControls->addZoomInput((event.pinch.scale - 1.0f) * 10.0f);
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const auto dt = static_cast<float>(
            static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq));
        prevCounter = nowCounter;

        // Advance the time of day, skipping the (boring) night by jumping 20:00 -> 05:00.
        if (animate) {
            hour += dt * TIME_SPEED;
            if (hour >= 20.0f) {
                hour -= 15.0f;
            }
        }

        // Sun: elevation from the time curve, azimuth sweeping east -> west across the day.
        const float elevation = elevationCurve.value(hour);
        const float azimuth = (hour / 24.0f) * 360.0f;
        const float el = elevation * DEG_TO_RAD;
        const float az = azimuth * DEG_TO_RAD;
        const float cosEl = std::cos(el);
        const Vector3 sunDir =
            Vector3(cosEl * std::sin(az), std::sin(el), cosEl * std::cos(az)).normalized();

        // The light shines FROM the sun, so its forward axis is the negated direction.
        sunEntity->lookAt(sunEntity->position() + (sunDir * -1.0f));

        // Sky luminance follows the elevation, exactly as upstream's curve does.
        const float luminance = luminanceCurve.value(elevation);
        AtmosphereData atmosphere;
        atmosphere.sunDirection[0] = sunDir.getX();
        atmosphere.sunDirection[1] = sunDir.getY();
        atmosphere.sunDirection[2] = sunDir.getZ();
        // DEVIATION: upstream's `luminance` curve is a Preetham control with no Nishita
        // counterpart — the scattering integral already darkens the sky as the sun sets.
        // The curve still drives the sun light's own intensity, so the ground lighting
        // follows the same day curve upstream uses.
        (void)luminance;
        scene->setAtmosphereUniforms(&atmosphere, sizeof(atmosphere));

        // Bloom intensity is driven by the sun elevation.
        if (cameraComponent) {
            const float bloom = bloomCurve.value(elevation);
            if (bloom != lastBloom) {
                auto rendering = cameraComponent->rendering();
                rendering.bloomIntensity = bloom;
                cameraComponent->setRendering(rendering);
                lastBloom = bloom;
            }
        }

        // Torches glow between sunset and sunrise.
        const float t = std::clamp((elevation + 3.0f) / 6.0f, 0.0f, 1.0f);
        const float nightFactor = 1.0f - (t * t * (3.0f - 2.0f * t));   // 1 - smoothstep(-3, 3, elevation)
        for (auto* torch : torchLights) {
            torch->setIntensity(TORCH_INTENSITY * nightFactor);
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();
    return 0;
}
