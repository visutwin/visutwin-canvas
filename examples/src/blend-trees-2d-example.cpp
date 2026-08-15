// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// 2D-cartesian animation blend-tree example — mirrors upstream's
// `animation/blend-trees-2d-cartesian` example. The skinned bitmoji character is
// driven through an AnimComponent state graph whose single "Emote" state hosts a
// BLEND_2D_CARTESIAN blend tree. FOUR leaf clips sit at 2D points on the blend
// plane, and TWO float parameters ("speedX", "speedY") pick the blend point;
// AnimBlendTreeCartesian2D computes the per-clip weights via gradient-band
// interpolation of the 2D positions.
//
// The idle/walk/run/win-dance clips live in SEPARATE GLB files
// (assets/animations/bitmoji/*.glb) and are retargeted onto the bitmoji rig by
// node name via DefaultAnimBinder — cross-GLB retargeting, same as the 1D
// anim-stategraph example this is based on.
//
// Blend tree layout (see build below):
//
//        speedY
//          ^
//    walk (0,1) ── run (1,1)
//          │          │
//    idle (0,0) ── dance (1,0) ─> speedX
//
// Auto-demo: the 2D blend point sweeps a circle around the centre of the point
// cloud (a Lissajous-ish path), so the character continuously blends between all
// four clips across the 2D plane.
//
// Keys: 1 = idle (0,0), 2 = walk (0,1), 3 = run (1,1), 4 = dance (1,0),
//       5 = centre (0.5,0.5 — even four-way blend). Any key stops the auto-demo.
//
// DEVIATION / NOTE: this is a valid 2D-cartesian blend demonstration, but the
// clip set here is forward-locomotion + a dance emote (idle/walk/run/win-dance),
// NOT upstream's directional strafe set (idle/eager/walk/dance arranged around
// the origin). So the 2D axes are illustrative of the blend feature rather than
// semantically directional (there is no left/right strafe clip). The blending
// math (AnimBlendTreeCartesian2D) is exercised identically regardless.
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
const auto danceAnimAsset = std::make_unique<Asset>(
    "danceAnim", AssetType::CONTAINER, rootPath + "/animations/bitmoji/win-dance.glb");

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

    spdlog::info("*** 2D-Cartesian Blend-Tree Example ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "2D Cartesian Blend Tree",
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
    // Match upstream blend-trees-2d-cartesian: exposure 2, full-intensity skybox,
    // default linear tone mapping (no ACES — it darkens midtones noticeably).
    scene->setSkyboxMip(2);
    scene->setExposure(2.0f);

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
    auto danceTrack = loadFirstAnimTrack(danceAnimAsset.get());

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

    // -----------------------------------------------------------------------
    // Build the anim state graph with a single 2D-CARTESIAN blend tree:
    //
    //   START ──► Emote { BLEND_2D_CARTESIAN, params [speedX, speedY],
    //                     leaves: Idle(0,0) Walk(0,1) Run(1,1) Dance(1,0) }
    //
    // The two float parameters name the X/Y axes of the blend plane; the leaf
    // `point`s are the clips' 2D positions. AnimBlendTreeCartesian2D derives each
    // clip's weight from where (speedX, speedY) falls among those points.
    // -----------------------------------------------------------------------
    AnimComponent* animComp = nullptr;
    {
        if (!idleTrack || !walkTrack || !runTrack || !danceTrack) {
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
        // Start the blend point at the centre of the cloud → even four-way blend.
        stateGraph.addFloatParameter("speedX", 0.5f);
        stateGraph.addFloatParameter("speedY", 0.5f);

        auto& layer = stateGraph.addLayer("base");

        // The 2D-cartesian blend tree. TWO driving parameters, and each leaf
        // child carries a distinct Vector2 `point` on the blend plane.
        AnimBlendTreeDesc emoteTree;
        emoteTree.type = AnimBlendType::BLEND_2D_CARTESIAN;
        emoteTree.parameters = {"speedX", "speedY"};
        emoteTree.syncAnimations = true;
        emoteTree.children.push_back(AnimBlendTreeDesc{.name = "Idle",  .point = Vector2(0.0f, 0.0f)});
        emoteTree.children.push_back(AnimBlendTreeDesc{.name = "Walk",  .point = Vector2(0.0f, 1.0f)});
        emoteTree.children.push_back(AnimBlendTreeDesc{.name = "Run",   .point = Vector2(1.0f, 1.0f)});
        emoteTree.children.push_back(AnimBlendTreeDesc{.name = "Dance", .point = Vector2(1.0f, 0.0f)});
        layer.states.push_back(AnimStateDesc{"Emote", 1.0f, true, emoteTree});

        layer.transitions.push_back(AnimTransitionDesc{.from = "START", .to = "Emote"});

        animComp = static_cast<AnimComponent*>(modelEntity->addComponent<AnimComponent>());
        animComp->loadStateGraph(stateGraph);
        // Assign tracks parsed from the SEPARATE animation GLBs — retargeted onto
        // the bitmoji rig by node name — to each blend-tree leaf via "State.Leaf".
        animComp->assignAnimation("Emote.Idle", idleTrack);
        animComp->assignAnimation("Emote.Walk", walkTrack);
        animComp->assignAnimation("Emote.Run", runTrack);
        animComp->assignAnimation("Emote.Dance", danceTrack);

        spdlog::info("2D blend tree loaded: Emote{{Idle(0,0) Walk(0,1) Run(1,1) Dance(1,0)}} "
                     "driven by (speedX, speedY) — keys 1-5 set the blend point");
    }

    // -----------------------------------------------------------------------
    // Ground plane (playcanvas-grey texture, like the upstream demos)
    // -----------------------------------------------------------------------
    {
        auto groundMaterial = std::make_shared<StandardMaterial>();
        const auto groundTexRes = groundTexAsset->resource();
        if (groundTexRes && std::holds_alternative<Texture*>(*groundTexRes)) {
            groundMaterial->setDiffuseMap(std::get<Texture*>(*groundTexRes));
        } else {
            spdlog::warn("Ground texture failed to load — using flat grey");
        }
        // Darken the ground so it stays below 1.0 under exposure 2 + linear
        // tonemap — a blown-out white ground clips the soft shadow away
        // (shadow only removes the directional term; IBL keeps the rest lit).
        groundMaterial->setDiffuse(Color(0.32f, 0.32f, 0.32f, 1.0f));

        auto* ground = new Entity();
        ground->setEngine(engine.get());
        ground->setLocalScale(15.0f, 1.0f, 15.0f);
        ground->setLocalPosition(0.0f, 0.0f, 0.0f);
        auto* groundRender = static_cast<RenderComponent*>(ground->addComponent<RenderComponent>());
        if (groundRender) {
            groundRender->setMaterial(groundMaterial.get());
            groundRender->setType("plane");
            // NOTE: the ground must stay a shadow CASTER — the directional
            // shadow camera tightens its far plane to the caster AABB union,
            // and a receiver-only ground would fall beyond it (shadow clips
            // along a light-space plane through the character).
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
        keyLightComp->setIntensity(1.5f);
        keyLightComp->setCastShadows(true);
        // Upstream shadow settings: distance 6, bias 0.02, normalOffsetBias 0.02,
        // single cascade (upstream default) — the tight fit keeps the whole
        // character silhouette in one cascade instead of splitting it.
        keyLightComp->setShadowResolution(2048);
        keyLightComp->setShadowDistance(20.0f);
        keyLightComp->setNumCascades(1);
        keyLightComp->setShadowBias(0.02f);
        keyLightComp->setShadowNormalBias(0.02f);
    }
    keyLight->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
    engine->root()->addChild(keyLight);

    // -----------------------------------------------------------------------
    // Camera with orbit controls
    // -----------------------------------------------------------------------
    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    cameraEntity->addComponent<ScriptComponent>();

    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
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

    spdlog::info("Controls: keys 1-5 pick blend corners/centre; LMB/RMB orbit, "
                 "Shift/MMB pan, Wheel zoom, F focus, R reset, Esc quit");

    // -----------------------------------------------------------------------
    // Main loop — engine->update(dt) advances the anim component, which
    // recomputes the 2D blend weights from (speedX, speedY) and composites the
    // four clips; GPU skinning follows the resulting bones each frame.
    // -----------------------------------------------------------------------
    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    // Auto-demo: sweep the 2D blend point around a circle centred on the point
    // cloud (0.5, 0.5) until the user presses a number key. The path visits the
    // neighbourhood of every leaf, so the character continuously cross-blends.
    bool autoDemo = true;
    float demoTime = 0.0f;

    // Manual key targets: the four leaf corners + the centre.
    static constexpr float keyX[] = {0.0f, 0.0f, 1.0f, 1.0f, 0.5f};
    static constexpr float keyY[] = {0.0f, 1.0f, 1.0f, 0.0f, 0.5f};
    static constexpr const char* keyLabels[] = {
        "idle (0,0)", "walk (0,1)", "run (1,1)", "dance (1,0)", "centre (0.5,0.5) even blend"};

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key >= SDLK_1 &&
                       event.key.key <= SDLK_5 && animComp) {
                const int i = event.key.key - SDLK_1;
                autoDemo = false;  // manual control takes over
                animComp->setFloat("speedX", keyX[i]);
                animComp->setFloat("speedY", keyY[i]);
                spdlog::info("blend point = ({:.2f}, {:.2f}) → {} (state: {})",
                    keyX[i], keyY[i], keyLabels[i],
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
            // Circle of radius 0.5 centred at (0.5, 0.5): X uses cos, Y uses sin
            // at a slightly different rate so the sweep isn't a perfect loop
            // (Lissajous-like), touching each corner's neighbourhood in turn.
            constexpr float kCenter = 0.5f;
            constexpr float kRadius = 0.5f;
            const float sx = kCenter + kRadius * std::cos(demoTime * 0.6f);
            const float sy = kCenter + kRadius * std::sin(demoTime * 0.8f);
            animComp->setFloat("speedX", sx);
            animComp->setFloat("speedY", sy);
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** 2D-Cartesian Blend-Tree Example Finished ***");

    return 0;
}
