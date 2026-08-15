// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Anim state-graph example — mirrors upstream's `locomotion` example. The
// skinned bitmoji character is driven through an AnimComponent state graph:
// an "Idle" state transitions into a 1D "Locomotion" blend tree (Walk <-> Run)
// based on a float "speed" parameter. The idle/walk/run animation clips live in
// SEPARATE GLB files (assets/animations/bitmoji/*.glb) and are retargeted onto
// the bitmoji rig by node name via DefaultAnimBinder — cross-GLB retargeting.
//
// Keys: 1 = idle (speed 0), 2 = walk (1.0), 3 = run (2.0), 4 = 50/50 blend (1.5).
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/animation/animationComponent.h"
#include "framework/components/animation/animationComponentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/entity.h"
#include "framework/parsers/glbContainerResource.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1200;
constexpr int WINDOW_HEIGHT = 800;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Character model (skinned rig) + the separate per-clip animation GLBs. Each
// animation GLB's curves target the shared bitmoji rig by node name, so the
// tracks retarget onto the instantiated character via DefaultAnimBinder.
const auto modelAsset = std::make_unique<Asset>(
    "bitmoji",
    AssetType::CONTAINER,
    rootPath + "/models/bitmoji.glb"
);

const auto idleAnimAsset = std::make_unique<Asset>(
    "idleAnim", AssetType::CONTAINER, rootPath + "/animations/bitmoji/idle.glb");
const auto walkAnimAsset = std::make_unique<Asset>(
    "walkAnim", AssetType::CONTAINER, rootPath + "/animations/bitmoji/walk.glb");
const auto runAnimAsset = std::make_unique<Asset>(
    "runAnim", AssetType::CONTAINER, rootPath + "/animations/bitmoji/run.glb");

const auto groundTexAsset = std::make_unique<Asset>(
    "playcanvas-grey",
    AssetType::TEXTURE,
    rootPath + "/textures/playcanvas-grey.png"
);

const auto envAtlas = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

// Load a container GLB and return its first parsed animation track (each
// bitmoji animation GLB contains exactly one clip). Returns nullptr on failure.
std::shared_ptr<AnimTrack> loadFirstAnimTrack(Asset* asset)
{
    if (!asset) {
        return nullptr;
    }
    const auto resource = asset->resource();
    if (!resource || !std::holds_alternative<ContainerResource*>(*resource)) {
        spdlog::error("Animation GLB '{}' failed to load as a container", asset->name());
        return nullptr;
    }
    auto* glb = dynamic_cast<GlbContainerResource*>(std::get<ContainerResource*>(*resource));
    if (!glb || glb->animTracks().empty()) {
        spdlog::error("Animation GLB '{}' contains no animation tracks", asset->name());
        return nullptr;
    }
    const auto& tracks = glb->animTracks();
    const auto& [name, track] = *tracks.begin();
    spdlog::info("Loaded animation track '{}' from '{}' ({} curves)",
        name, asset->name(), track ? track->curves().size() : 0);
    return track;
}

BoundingBox calcEntityAABB(Entity* entity)
{
    BoundingBox bbox;
    bbox.setCenter(0.0f, 0.0f, 0.0f);
    bbox.setHalfExtents(0.0f, 0.0f, 0.0f);

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
}

int main()
{
    log::init();
    log::set_level_debug();

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

    spdlog::info("*** Animation Example — A Windy Day ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "A Windy Day",
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        spdlog::error("SDL Window Creation Failed");
        shutdown();
        return -1;
    }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        spdlog::error("SDL Renderer Creation Failed");
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        spdlog::error("Unable to get render Metal layer");
        shutdown();
        return -1;
    }

    auto device = createGraphicsDevice(
        GraphicsDeviceOptions{.swapChain = swapchain, .window = window}
    );
    if (!device) {
        spdlog::error("Unable to create graphics device");
        shutdown();
        return -1;
    }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<AnimationComponentSystem>();
    createOptions.registerComponentSystem<AnimComponentSystem>();
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
    scene->setSkyboxMip(2);
    scene->setSkyboxIntensity(0.7f);
    scene->setExposure(1.0f);
    scene->setToneMapping(TONEMAP_ACES);

    // Load environment atlas for IBL
    const auto envAtlasResource = envAtlas->resource();
    if (envAtlasResource) {
        scene->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
    } else {
        spdlog::warn("Failed to load environment atlas — continuing without IBL");
    }

    // -----------------------------------------------------------------------
    // Load the bitmoji character model
    // -----------------------------------------------------------------------
    spdlog::info("Loading bitmoji character GLB...");
    const auto resource = modelAsset->resource();
    if (!resource) {
        spdlog::error("GLB load failed: asset resource is null");
        shutdown();
        return -1;
    }
    if (!std::holds_alternative<ContainerResource*>(*resource)) {
        spdlog::error("GLB load failed: expected ContainerResource");
        shutdown();
        return -1;
    }

    auto* container = std::get<ContainerResource*>(*resource);
    if (!container) {
        spdlog::error("GLB load failed: container payload is null");
        shutdown();
        return -1;
    }

    auto* modelEntity = container->instantiateRenderEntity();
    if (!modelEntity) {
        spdlog::error("GLB instantiate failed");
        shutdown();
        return -1;
    }
    modelEntity->setEngine(engine.get());
    engine->root()->addChild(modelEntity);

    // Load the separate per-clip animation GLBs and extract their tracks. These
    // will be retargeted onto the bitmoji rig by node name.
    auto idleTrack = loadFirstAnimTrack(idleAnimAsset.get());
    auto walkTrack = loadFirstAnimTrack(walkAnimAsset.get());
    auto runTrack = loadFirstAnimTrack(runAnimAsset.get());

    // Log model stats
    {
        int renderComps = 0, meshInsts = 0;
        for (auto* render : RenderComponent::instances()) {
            if (!render || !render->entity()) continue;
            auto* owner = render->entity();
            if (owner != modelEntity && !owner->isDescendantOf(modelEntity)) continue;
            renderComps++;
            meshInsts += static_cast<int>(render->meshInstances().size());
        }
        spdlog::info("Model loaded: {} render components, {} mesh instances",
            renderComps, meshInsts);
    }

    // Build the anim state graph (mirrors upstream locomotion):
    //
    //   START ──► Idle ──(speed >= 0.5)──► Locomotion (1D blend: Walk@1 .. Run@2)
    //              ▲                            │
    //              └───────(speed < 0.5)────────┘
    //
    AnimComponent* animComp = nullptr;
    {
        if (!idleTrack || !walkTrack || !runTrack) {
            spdlog::error("Missing one or more animation tracks — cannot build state graph");
            shutdown();
            return -1;
        }

        // The bitmoji GLB has no embedded animations, but disable any legacy
        // AnimationComponent just in case so it doesn't fight the state graph.
        if (auto* legacyAnim = modelEntity->findComponent<AnimationComponent>()) {
            legacyAnim->setPlaying(false);
            legacyAnim->setEnabled(false);
        }

        AnimStateGraph stateGraph;
        stateGraph.addFloatParameter("speed", 0.0f);

        auto& layer = stateGraph.addLayer("locomotion");
        layer.states.push_back(AnimStateDesc{"Idle"});

        AnimBlendTreeDesc locomotionTree;
        locomotionTree.type = AnimBlendType::BLEND_1D;
        locomotionTree.parameters = {"speed"};
        locomotionTree.syncAnimations = true;
        locomotionTree.children.push_back(AnimBlendTreeDesc{.name = "Walk", .point = Vector2(1.0f, 0.0f)});
        locomotionTree.children.push_back(AnimBlendTreeDesc{.name = "Run", .point = Vector2(2.0f, 0.0f)});
        layer.states.push_back(AnimStateDesc{"Locomotion", 1.0f, true, locomotionTree});

        layer.transitions.push_back(AnimTransitionDesc{.from = "START", .to = "Idle"});
        layer.transitions.push_back(AnimTransitionDesc{
            .from = "Idle", .to = "Locomotion", .time = 0.4f,
            .conditions = {{"speed", AnimPredicate::GREATER_THAN_EQUAL_TO, 0.5f}}});
        layer.transitions.push_back(AnimTransitionDesc{
            .from = "Locomotion", .to = "Idle", .time = 0.4f,
            .conditions = {{"speed", AnimPredicate::LESS_THAN, 0.5f}}});

        animComp = static_cast<AnimComponent*>(modelEntity->addComponent<AnimComponent>());
        animComp->loadStateGraph(stateGraph);
        // Assign tracks parsed from the SEPARATE animation GLBs — retargeted onto
        // the bitmoji rig by node name.
        animComp->assignAnimation("Idle", idleTrack);
        animComp->assignAnimation("Locomotion.Walk", walkTrack);
        animComp->assignAnimation("Locomotion.Run", runTrack);

        spdlog::info("State graph loaded: states [Idle, Locomotion(Walk|Run)] — keys 1/2/3/4 set speed");
    }

    // -----------------------------------------------------------------------
    // Ground plane (playcanvas-grey texture, like the upstream locomotion demo)
    // -----------------------------------------------------------------------
    {
        auto groundMaterial = std::make_shared<StandardMaterial>();
        const auto groundTexRes = groundTexAsset->resource();
        if (groundTexRes && std::holds_alternative<Texture*>(*groundTexRes)) {
            groundMaterial->setDiffuseMap(std::get<Texture*>(*groundTexRes));
        } else {
            groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.5f, 1.0f));
            spdlog::warn("Ground texture failed to load — using flat grey");
        }

        auto* ground = new Entity();
        ground->setEngine(engine.get());
        ground->setLocalScale(15.0f, 1.0f, 15.0f);
        ground->setLocalPosition(0.0f, 0.0f, 0.0f);
        auto* groundRender = static_cast<RenderComponent*>(ground->addComponent<RenderComponent>());
        if (groundRender) {
            groundRender->setMaterial(groundMaterial.get());
            groundRender->setType("plane");
        }
        engine->root()->addChild(ground);
        // Keep the material alive for the lifetime of the app.
        static std::shared_ptr<StandardMaterial> keepAlive = groundMaterial;
    }

    // Auto-frame the model
    const BoundingBox modelBounds = calcEntityAABB(modelEntity);
    const Vector3 center = modelBounds.center();
    const Vector3 halfExt = modelBounds.halfExtents();
    const float radius = std::max(halfExt.length(), 0.5f);
    spdlog::info("Model bounds: center=({:.2f}, {:.2f}, {:.2f}), half=({:.2f}, {:.2f}, {:.2f}), radius={:.2f}",
        center.getX(), center.getY(), center.getZ(),
        halfExt.getX(), halfExt.getY(), halfExt.getZ(), radius);

    // -----------------------------------------------------------------------
    // Lights
    // -----------------------------------------------------------------------
    auto* keyLight = new Entity();
    keyLight->setEngine(engine.get());
    auto* keyLightComp = static_cast<LightComponent*>(keyLight->addComponent<LightComponent>());
    if (keyLightComp) {
        keyLightComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        keyLightComp->setColor(Color(1.0f, 0.97f, 0.92f));
        keyLightComp->setIntensity(1.5f);
        keyLightComp->setCastShadows(true);
        keyLightComp->setShadowResolution(2048);
        keyLightComp->setShadowDistance(std::max(radius * 4.0f, 100.0f));
        keyLightComp->setShadowBias(0.3f);
        keyLightComp->setShadowNormalBias(0.05f);
    }
    keyLight->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
    engine->root()->addChild(keyLight);

    auto* fillLight = new Entity();
    fillLight->setEngine(engine.get());
    auto* fillLightComp = static_cast<LightComponent*>(fillLight->addComponent<LightComponent>());
    if (fillLightComp) {
        fillLightComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        fillLightComp->setColor(Color(0.65f, 0.75f, 1.0f));
        fillLightComp->setIntensity(0.5f);
    }
    fillLight->setLocalEulerAngles(-20.0f, -150.0f, 0.0f);
    engine->root()->addChild(fillLight);

    // -----------------------------------------------------------------------
    // Camera with orbit controls
    // -----------------------------------------------------------------------
    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    cameraEntity->addComponent<ScriptComponent>();

    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setClearColor(Color(0.05f, 0.05f, 0.08f, 1.0f));
        cameraComp->camera()->setFov(55.0f);
        cameraComp->camera()->setNearClip(std::max(0.01f, radius * 0.005f));
        cameraComp->camera()->setFarClip(std::max(500.0f, radius * 20.0f));
    }

    const float camDistance = std::max(radius * 2.8f, 5.0f);
    cameraEntity->setLocalPosition(center + Vector3(0.0f, radius * 0.5f, camDistance));
    engine->root()->addChild(cameraEntity);

    auto* cameraControls = cameraEntity->script()->create<CameraControls>();
    cameraControls->setFocusPoint(center);
    cameraControls->setEnableFly(false);
    cameraControls->setMoveSpeed(radius);
    cameraControls->setMoveFastSpeed(radius * 2.0f);
    cameraControls->setMoveSlowSpeed(radius * 0.5f);
    cameraControls->setOrbitDistance(camDistance);
    cameraControls->storeResetState();

    spdlog::info("Controls: LMB/RMB orbit, Shift/MMB pan, Wheel zoom, F focus, R reset, Esc quit");

    // -----------------------------------------------------------------------
    // Main loop — engine->update(dt) advances the animation, which drives the
    // joint entities; GPU skinning follows the bones each frame.
    // -----------------------------------------------------------------------
    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    // Auto-demo: cycle the speed parameter until the user presses a number key.
    bool autoDemo = true;
    float demoTime = 0.0f;
    int demoStep = -1;
    static constexpr float demoSpeeds[] = {0.0f, 2.0f, 1.5f, 1.0f};
    static constexpr const char* demoLabels[] = {"idle (Survey)", "run", "walk/run 50/50 blend", "walk"};

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key >= SDLK_1 &&
                       event.key.key <= SDLK_4 && animComp) {
                static constexpr float speeds[] = {0.0f, 1.0f, 2.0f, 1.5f};
                const float speed = speeds[event.key.key - SDLK_1];
                autoDemo = false;  // manual control takes over
                animComp->setFloat("speed", speed);
                spdlog::info("speed = {:.1f} (state: {})", speed,
                    animComp->baseLayer() ? animComp->baseLayer()->activeState() : "?");
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(center, camDistance);
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();
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

        if (autoDemo && animComp) {
            demoTime += dt;
            const int step = static_cast<int>(demoTime / 4.0f) % 4;
            if (step != demoStep) {
                demoStep = step;
                animComp->setFloat("speed", demoSpeeds[step]);
                spdlog::info("[auto-demo] speed = {:.1f} → {} (state: {})", demoSpeeds[step],
                    demoLabels[step],
                    animComp->baseLayer() ? animComp->baseLayer()->activeState() : "?");
            }
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** Animation Example Finished ***");

    return 0;
}
