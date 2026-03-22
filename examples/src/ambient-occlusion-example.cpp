// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <core/shape/boundingBox.h>
#include <framework/assets/asset.h>

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/constants.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/animation/animationComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "platform/graphics/depthState.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Assets matching upstream AO example
const auto envAtlas = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

const auto laboratory = std::make_unique<Asset>(
    "laboratory",
    AssetType::CONTAINER,
    rootPath + "/models/da_vinci_workshop.glb"
);

const auto leonardoBust = std::make_unique<Asset>(
    "leonardo",
    AssetType::CONTAINER,
    rootPath + "/models/leonardo_da_vinci.glb"
);

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
        bbox.setHalfExtents(0.5f, 0.5f, 0.5f);
    }
    return bbox;
};

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

    spdlog::info("*** VisuTwin Ambient Occlusion Example Started ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Ambient Occlusion Example", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    createOptions.registerComponentSystem<AnimationComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);

    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);

    engine->start();

    auto scene = engine->scene();

    // setup skydome
    scene->setSkyboxMip(2);
    scene->setExposure(1.5f);
    scene->setToneMapping(TONEMAP_NEUTRAL);

    const auto envAtlasResource = envAtlas->resource();
    if (!envAtlasResource) {
        spdlog::error("Failed to load environment atlas texture");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*envAtlasResource));

    // create laboratory entity — auto-scale to fit scene
    const auto labResource = laboratory->resource();
    if (!labResource) {
        spdlog::error("Failed to load laboratory model");
        shutdown();
        return -1;
    }
    auto labEntity = std::get<ContainerResource*>(*labResource)->instantiateRenderEntity();
    engine->root()->addChild(labEntity);

    // Normalize model so its longest extent is ~100 units
    {
        const auto bbox = calcEntityAABB(labEntity);
        const auto& he = bbox.halfExtents();
        const auto& ct = bbox.center();
        const float maxExtent = std::max({he.getX(), he.getY(), he.getZ()}) * 2.0f;
        if (maxExtent > 0.001f) {
            const float s = 100.0f / maxExtent;
            labEntity->setLocalScale(s, s, s);
            labEntity->setLocalPosition(
                -ct.getX() * s,
                -ct.getY() * s + he.getY() * s + (-40.0f),
                -ct.getZ() * s);
            spdlog::info("Laboratory model: extent={:.1f}, scale={:.3f}", maxExtent, s);
        }
    }

    // set up materials — enable shadows, disable baked AO, disable blending
    for (auto* render : RenderComponent::instances()) {
        if (!render || !render->entity()) continue;
        auto* owner = render->entity();
        if (owner != labEntity && !owner->isDescendantOf(labEntity)) continue;

        render->setCastShadows(true);
        render->setReceiveShadows(true);

        for (auto* mi : render->meshInstances()) {
            if (!mi || !mi->material()) continue;
            auto* mat = dynamic_cast<StandardMaterial*>(mi->material());
            if (!mat) continue;
            // disable baked AO map — we want SSAO only
            mat->setAoMap(nullptr);
            // disable blending / enable depth writes
            mat->setTransparent(false);
            mat->setDepthState(std::make_shared<DepthState>());
        }
    }

    // add Leonardo da Vinci bust next to the workshop
    Entity* leoEntity = nullptr;
    {
        const auto leoResource = leonardoBust->resource();
        if (leoResource) {
            leoEntity = std::get<ContainerResource*>(*leoResource)->instantiateRenderEntity();
            engine->root()->addChild(leoEntity);

            // Scale bust to same normalized size as the workshop (100 units)
            const auto leoBbox = calcEntityAABB(leoEntity);
            const auto& leoHe = leoBbox.halfExtents();
            const auto& leoCt = leoBbox.center();
            const float leoMaxExtent = std::max({leoHe.getX(), leoHe.getY(), leoHe.getZ()}) * 2.0f;
            if (leoMaxExtent > 0.001f) {
                const float leoScale = 150.0f / leoMaxExtent;
                leoEntity->setLocalScale(leoScale, leoScale, leoScale);

                // Place to the right of the workshop, sitting on the ground plane
                const auto labBbox = calcEntityAABB(labEntity);
                const float offsetX = labBbox.center().getX() + labBbox.halfExtents().getX() + 100.0f;
                leoEntity->setLocalPosition(
                    offsetX - leoCt.getX() * leoScale,
                    -leoCt.getY() * leoScale + leoHe.getY() * leoScale + (-40.0f),
                    -leoCt.getZ() * leoScale);

                // Rotate 45 degrees to face toward the Mona Lisa portrait
                leoEntity->setLocalEulerAngles(0.0f, 135.0f, 0.0f);

                spdlog::info("Leonardo bust: extent={:.1f}, scale={:.3f}", leoMaxExtent, leoScale);
            }

            // Enable shadows on the bust
            for (auto* render : RenderComponent::instances()) {
                if (!render || !render->entity()) continue;
                auto* owner = render->entity();
                if (owner != leoEntity && !owner->isDescendantOf(leoEntity)) continue;
                render->setCastShadows(true);
                render->setReceiveShadows(true);
            }
        } else {
            spdlog::warn("Leonardo bust model not found — skipping");
        }
    }

    // Center both objects on the ground plane (X=0, Z=0)
    {
        auto combined = calcEntityAABB(labEntity);
        if (leoEntity) combined.add(calcEntityAABB(leoEntity));
        const float shiftX = -combined.center().getX();
        const float shiftZ = -combined.center().getZ();
        if (std::abs(shiftX) > 0.01f || std::abs(shiftZ) > 0.01f) {
            auto lp = labEntity->position();
            labEntity->setLocalPosition(lp.getX() + shiftX, lp.getY(), lp.getZ() + shiftZ);
            if (leoEntity) {
                auto leoP = leoEntity->position();
                leoEntity->setLocalPosition(leoP.getX() + shiftX, leoP.getY(), leoP.getZ() + shiftZ);
            }
        }
    }

    // add fill point lights around the model
    {
        const auto bbox = calcEntityAABB(labEntity);
        const auto& ct = bbox.center();
        const float r = bbox.halfExtents().length() * 0.6f;
        const float h = ct.getY() + bbox.halfExtents().getY() * 0.5f;

        struct LightDef { float dx; float dz; Color color; };
        const LightDef fills[] = {
            { -r,  r, Color(1.0f, 0.85f, 0.6f) },
            {  r, -r, Color(0.6f, 0.8f, 1.0f)  },
        };
        for (auto& def : fills) {
            auto* pl = new Entity();
            pl->setEngine(engine.get());
            auto* lc = static_cast<LightComponent*>(pl->addComponent<LightComponent>());
            if (lc) {
                lc->setType(LightType::LIGHTTYPE_OMNI);
                lc->setColor(def.color);
                lc->setIntensity(1.0f);
                lc->setRange(r * 4.0f);
                lc->setCastShadows(false);
            }
            pl->setLocalPosition(ct.getX() + def.dx, h, ct.getZ() + def.dz);
            engine->root()->addChild(pl);
        }
    }

    // add a spotlight on the Leonardo portrait (museum-style exhibition light)
    // Light direction is entity's -Y axis. No rotation = straight down.
    // Small X rotation tilts the beam forward for dramatic angle.
    LightComponent* spotComp = nullptr;
    if (leoEntity) {
        const auto leoBbox = calcEntityAABB(leoEntity);
        const auto& leoCt = leoBbox.center();
        const float leoHeight = leoBbox.halfExtents().getY() * 2.0f;

        auto* spot = new Entity();
        spot->setEngine(engine.get());
        spotComp = static_cast<LightComponent*>(spot->addComponent<LightComponent>());
        if (spotComp) {
            spotComp->setType(LightType::LIGHTTYPE_SPOT);
            spotComp->setColor(Color(1.0f, 0.95f, 0.8f));  // warm white
            spotComp->setIntensity(8.0f);
            spotComp->setRange(leoHeight * 6.0f);
            spotComp->setInnerConeAngle(10.0f);
            spotComp->setOuterConeAngle(20.0f);
            spotComp->setCastShadows(true);
            spotComp->setShadowBias(0.3f);
            spotComp->setShadowNormalBias(0.05f);
            spotComp->setShadowResolution(2048);
        }
        // Position above the bust; -Y axis points straight down (no rotation needed).
        // Tilt 15° on X to angle slightly forward for a museum look.
        // Position above-front-right of the bust, aimed at its center.
        // Light direction = entity -Y axis.
        const float spotOffset = leoHeight * 1.0f;
        const float spotX = leoCt.getX() + spotOffset * 0.5f;
        const float spotY = leoCt.getY() + leoHeight * 1.5f;
        const float spotZ = leoCt.getZ() + spotOffset * 0.5f;
        spot->setLocalPosition(spotX, spotY, spotZ);

        // Aim -Y axis at bust center using quaternion from axis-angle.
        // Direction from spot to target (normalized):
        // Aim at the top of the bust (portrait/face area)
        const float targetY = leoCt.getY() + leoHeight * 0.5f;
        float dirX = leoCt.getX() - spotX;
        float dirY = targetY - spotY;
        float dirZ = leoCt.getZ() - spotZ;
        const float dirLen = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
        if (dirLen > 0.001f) {
            dirX /= dirLen; dirY /= dirLen; dirZ /= dirLen;
            // Rotate from=(0,-1,0) to dir. Quaternion(x,y,z,w) order.
            // cross(from, dir) = cross((0,-1,0), (dx,dy,dz)) = (-dz, 0, dx)
            // dot(from, dir) = -dy
            const float dot = -dirY;
            const float axX = -dirZ;
            const float axZ = dirX;
            const float s = std::sqrt((1.0f + dot) * 2.0f);
            const float invS = 1.0f / s;
            // Quaternion(x, y, z, w)
            spot->setLocalRotation(Quaternion(axX * invS, 0.0f, axZ * invS, s * 0.5f));
        }
        engine->root()->addChild(spot);

        spdlog::info("Spotlight: pos=({:.0f},{:.0f},{:.0f}) -> target=({:.0f},{:.0f},{:.0f}) dir=({:.2f},{:.2f},{:.2f})",
            spotX, spotY, spotZ, leoCt.getX(), leoCt.getY(), leoCt.getZ(), dirX, dirY, dirZ);
        engine->root()->addChild(spot);
        spdlog::info("Spotlight on Leonardo: pos=({:.0f}, {:.0f}, {:.0f}), range={:.0f}",
            leoCt.getX(), leoCt.getY() + leoHeight * 1.5f, leoCt.getZ(), leoHeight * 4.0f);
    }

    // add a ground plane
    {
        auto* planeMaterial = new StandardMaterial();
        planeMaterial->setDiffuse(Color(0.2f, 0.2f, 0.2f));

        auto plane = new Entity();
        plane->setEngine(engine.get());
        auto* planeRender = static_cast<RenderComponent*>(plane->addComponent<RenderComponent>());
        if (planeRender) {
            planeRender->setMaterial(planeMaterial);
            planeRender->setType("plane");
        }
        plane->setLocalScale(400.0f, 1.0f, 400.0f);
        plane->setLocalPosition(0.0f, -40.0f, 0.0f);
        engine->root()->addChild(plane);
    }

    // add shadow casting directional light
    auto light = new Entity();
    light->setEngine(engine.get());
    auto* lightComp = static_cast<LightComponent*>(light->addComponent<LightComponent>());
    if (lightComp) {
        lightComp->setCastShadows(true);
        lightComp->setShadowResolution(4096);
        lightComp->setShadowDistance(600.0f);
        lightComp->setShadowBias(0.4f);
        lightComp->setShadowNormalBias(0.06f);
    }
    light->setLocalEulerAngles(35, 30, 0);
    engine->root()->addChild(light);

    // create camera entity
    auto camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();

    // nearClip=1, farClip=600
    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setNearClip(1.0f);
        cameraComp->camera()->setFarClip(600.0f);
    }

    // enable SSAO
    if (cameraComp) {
        auto ssao = cameraComp->ssao();
        ssao.enabled = true;
        ssao.blurEnabled = true;
        ssao.radius = 30.0f;
        ssao.samples = 12;
        ssao.intensity = 0.4f;
        ssao.power = 6.0f;
        ssao.minAngle = 10.0f;
        ssao.scale = 1.0f;
        ssao.randomize = false;
        cameraComp->setSsao(ssao);

        // tone mapping
        auto rendering = cameraComp->rendering();
        rendering.toneMapping = TONEMAP_NEUTRAL;
        cameraComp->setRendering(rendering);
    }

    camera->setPosition(Vector3(-60.0f, 30.0f, 60.0f));
    engine->root()->addChild(camera);

    // Setup orbit camera controls — focus on combined scene bounds
    auto sceneBbox = calcEntityAABB(labEntity);
    if (leoEntity) {
        const auto leoBbox = calcEntityAABB(leoEntity);
        sceneBbox.add(leoBbox);
    }
    auto* cameraControls = camera->script()->create<CameraControls>();
    const float sceneRadius = std::max(sceneBbox.halfExtents().length(), 1.0f);

    cameraControls->setFocusPoint(sceneBbox.center());
    cameraControls->setEnableFly(false);
    cameraControls->setMoveSpeed(2 * sceneRadius);
    cameraControls->setMoveFastSpeed(4 * sceneRadius);
    cameraControls->setMoveSlowSpeed(sceneRadius);
    cameraControls->setOrbitDistance(std::max(sceneRadius * 2.0f, 200.0f));
    cameraControls->setAutoFarClip(true, 10.0f, 1000.0f);
    cameraControls->storeResetState();

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    // Helper lambdas for logging state
    auto logSsaoState = [&](const char* reason) {
        if (!cameraComp) {
            return;
        }
        const auto& ssao = cameraComp->ssao();
        spdlog::info("SSAO {}: enabled={}, blur={}, intensity={:.2f}, power={:.1f}, radius={:.1f}, samples={}, minAngle={:.1f}, scale={:.2f}, randomize={}",
            reason,
            ssao.enabled ? "ON" : "OFF",
            ssao.blurEnabled ? "ON" : "OFF",
            ssao.intensity,
            ssao.power,
            ssao.radius,
            ssao.samples,
            ssao.minAngle,
            ssao.scale,
            ssao.randomize ? "ON" : "OFF");
    };

    spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
    spdlog::info("SSAO controls: O toggle SSAO, B toggle blur, Z toggle randomize");
    spdlog::info("  +/- adjust intensity, [/] adjust radius, ,/. adjust samples, ;/' adjust power");
    logSsaoState("init");

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;

            // SSAO controls
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_O && cameraComp) {
                auto ssao = cameraComp->ssao();
                ssao.enabled = !ssao.enabled;
                cameraComp->setSsao(ssao);
                logSsaoState("toggle");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_B && cameraComp) {
                auto ssao = cameraComp->ssao();
                ssao.blurEnabled = !ssao.blurEnabled;
                cameraComp->setSsao(ssao);
                logSsaoState("blur");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_Z && cameraComp) {
                auto ssao = cameraComp->ssao();
                ssao.randomize = !ssao.randomize;
                cameraComp->setSsao(ssao);
                logSsaoState("randomize");

            // Intensity +/-
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_EQUALS && cameraComp) {
                auto ssao = cameraComp->ssao();
                ssao.intensity = std::min(1.0f, ssao.intensity + 0.05f);
                cameraComp->setSsao(ssao);
                logSsaoState("intensity+");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_MINUS && cameraComp) {
                auto ssao = cameraComp->ssao();
                ssao.intensity = std::max(0.0f, ssao.intensity - 0.05f);
                cameraComp->setSsao(ssao);
                logSsaoState("intensity-");

            // Radius [/]
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_RIGHTBRACKET && cameraComp) {
                auto ssao = cameraComp->ssao();
                ssao.radius = std::min(100.0f, ssao.radius + 5.0f);
                cameraComp->setSsao(ssao);
                logSsaoState("radius+");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_LEFTBRACKET && cameraComp) {
                auto ssao = cameraComp->ssao();
                ssao.radius = std::max(1.0f, ssao.radius - 5.0f);
                cameraComp->setSsao(ssao);
                logSsaoState("radius-");

            // Samples ,/.
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_PERIOD && cameraComp) {
                auto ssao = cameraComp->ssao();
                ssao.samples = std::min(32, ssao.samples + 2);
                cameraComp->setSsao(ssao);
                logSsaoState("samples+");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_COMMA && cameraComp) {
                auto ssao = cameraComp->ssao();
                ssao.samples = std::max(2, ssao.samples - 2);
                cameraComp->setSsao(ssao);
                logSsaoState("samples-");

            // Power ;/'
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_APOSTROPHE && cameraComp) {
                auto ssao = cameraComp->ssao();
                ssao.power = std::min(16.0f, ssao.power + 1.0f);
                cameraComp->setSsao(ssao);
                logSsaoState("power+");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SEMICOLON && cameraComp) {
                auto ssao = cameraComp->ssao();
                ssao.power = std::max(0.5f, ssao.power - 1.0f);
                cameraComp->setSsao(ssao);
                logSsaoState("power-");

            // Camera controls
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(sceneBbox.center(), std::max(sceneRadius * 2.0f, 200.0f));
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();

            // Spotlight toggle
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_L && spotComp) {
                spotComp->setEnabled(!spotComp->enabled());
                spdlog::info("Spotlight: {}", spotComp->enabled() ? "ON" : "OFF");

            // Zoom
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            } else if (event.type == SDL_EVENT_PINCH_UPDATE && cameraControls) {
                const float pinchDelta = (event.pinch.scale - 1.0f) * 10.0f;
                cameraControls->addZoomInput(pinchDelta);
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;
        const float dt = static_cast<float>(dtSeconds);

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** VisuTwin Ambient Occlusion Example Finished ***");

    return 0;
}
