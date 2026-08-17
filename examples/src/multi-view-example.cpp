// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Multi-view — port of upstream graphics/multi-view. One chess board rendered by
// three cameras into three viewports of the same back buffer:
//   * TOP (full width, upper half) — perspective, World layer, directional light.
//   * BOTTOM-LEFT                  — perspective, World layer, orbiting the board.
//   * BOTTOM-RIGHT                 — ORTHOGRAPHIC top-down, on a private
//                                    SpotLightLayer, so it sees only the yellow
//                                    spot light and not the directional one.
// The board belongs to both lighting layers, which is what lets the same geometry
// be lit differently per camera. Press D to step the debug shader pass applied to
// the top and right viewports (upstream exposes the same thing as a HUD dropdown).
//
// Model: "Chess Board" by Idmental, CC BY 4.0
// https://sketchfab.com/3d-models/chess-board-901eeeca884f4622ac37b7e8f7cb82c3
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
#include <vector>

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
#include "framework/handlers/containerResource.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/composition/layerComposition.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1200;
constexpr int WINDOW_HEIGHT = 760;
constexpr int LAYERID_SPOTLIGHT = 70;

using namespace visutwin::canvas;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;

const std::string rootPath = ASSET_DIR;

const auto helipad = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

void setRenderLayersRecursive(GraphNode* node, const std::vector<int>& layers)
{
    if (!node) {
        return;
    }
    if (auto* entity = dynamic_cast<Entity*>(node)) {
        if (auto* render = entity->findComponent<RenderComponent>()) {
            render->setLayers(layers);
        }
    }
    for (const auto& child : node->children()) {
        setRenderLayersRecursive(child.get(), layers);
    }
}

void setRenderShadowsRecursive(GraphNode* node, const bool cast, const bool receive)
{
    if (!node) {
        return;
    }
    if (auto* entity = dynamic_cast<Entity*>(node)) {
        if (auto* render = entity->findComponent<RenderComponent>()) {
            render->setCastShadows(cast);
            render->setReceiveShadows(receive);
        }
    }
    for (const auto& child : node->children()) {
        setRenderShadowsRecursive(child.get(), cast, receive);
    }
}

BoundingBox calcEntityAABB(Entity* entity)
{
    BoundingBox bbox;
    bbox.setCenter(0, 0, 0);
    bbox.setHalfExtents(0, 0, 0);
    if (!entity) return bbox;
    bool hasAny = false;
    for (auto* render : RenderComponent::instances()) {
        if (!render || !render->entity()) continue;
        auto* owner = render->entity();
        if (owner != entity && !owner->isDescendantOf(entity)) continue;
        for (auto* mi : render->meshInstances()) {
            if (!mi) continue;
            if (!hasAny) { bbox = mi->aabb(); hasAny = true; }
            else { bbox.add(mi->aabb()); }
        }
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

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Multi-View Control Room",
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    scene->setSkyboxMip(1);

    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad env atlas");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    // Layer setup: world + dedicated spotlight layer + skybox.
    const auto defaultLayers = scene->layers();
    const auto worldLayer = defaultLayers ? defaultLayers->getLayerById(LAYERID_WORLD) : nullptr;
    const auto skyboxLayer = defaultLayers ? defaultLayers->getLayerById(LAYERID_SKYBOX) : nullptr;
    const auto immediateLayer = defaultLayers ? defaultLayers->getLayerById(LAYERID_IMMEDIATE) : nullptr;
    const auto uiLayer = defaultLayers ? defaultLayers->getLayerById(LAYERID_UI) : nullptr;
    if (!worldLayer || !skyboxLayer) {
        spdlog::error("Failed to resolve required default layers");
        shutdown();
        return -1;
    }

    auto spotLightLayer = std::make_shared<Layer>("SpotLightLayer", LAYERID_SPOTLIGHT);
    auto composition = std::make_shared<LayerComposition>("multi-view");
    composition->pushOpaque(spotLightLayer);
    composition->pushOpaque(worldLayer);
    composition->pushOpaque(skyboxLayer);
    composition->pushTransparent(worldLayer);
    composition->pushTransparent(spotLightLayer);
    if (immediateLayer) {
        composition->pushOpaque(immediateLayer);
        composition->pushTransparent(immediateLayer);
    }
    if (uiLayer) {
        composition->pushTransparent(uiLayer);
    }
    scene->setLayers(composition);

    // Load the chess-board GLB once. Like upstream, it belongs to BOTH lighting
    // layers, so the world (left/top) cameras light it with the directional light
    // and the spotlight (right) camera lights it with the spot light.
    auto boardAsset = std::make_unique<Asset>(
        "chess-board",
        AssetType::CONTAINER,
        rootPath + "/models/chess-board.glb"
    );
    const auto boardResource = boardAsset->resource();
    if (!boardResource || !std::holds_alternative<ContainerResource*>(*boardResource)) {
        spdlog::error("Failed to load chess-board.glb");
        shutdown();
        return -1;
    }

    auto* boardContainer = std::get<ContainerResource*>(*boardResource);
    auto* boardEntity = boardContainer ? boardContainer->instantiateRenderEntity() : nullptr;
    if (!boardEntity) {
        spdlog::error("Failed to instantiate chess-board.glb render entity");
        shutdown();
        return -1;
    }
    boardEntity->setEngine(engine.get());
    // Both lighting layers, so each camera lights the same geometry from its own
    // light. The board keeps its authored size (~337 units across) — the camera
    // distances and ortho heights below are all framed against that, so scaling it
    // down to a "tidy" 100 units, as this port used to, pushed every viewport out
    // to a distant wide shot instead of upstream's close-ups.
    setRenderLayersRecursive(boardEntity, {LAYERID_WORLD, LAYERID_SPOTLIGHT});
    setRenderShadowsRecursive(boardEntity, true, true);
    engine->root()->addChild(boardEntity);

    // Left camera: perspective, bottom-left viewport, World layer.
    auto* leftCamEntity = new Entity();
    leftCamEntity->setEngine(engine.get());
    auto* leftCam = static_cast<CameraComponent*>(leftCamEntity->addComponent<CameraComponent>());
    if (leftCam && leftCam->camera()) {
        leftCam->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
        leftCam->camera()->setRect(Vector4(0.0f, 0.0f, 0.5f, 0.5f));
        leftCam->camera()->setScissorRect(Vector4(0.0f, 0.0f, 0.5f, 0.5f));
        leftCam->camera()->setFarClip(500.0f);
        leftCam->setToneMapping(TONEMAP_ACES);
    }
    leftCamEntity->setLocalPosition(100.0f, 35.0f, 100.0f);
    leftCamEntity->lookAt(Vector3(0.0f, 0.0f, 0.0f));
    engine->root()->addChild(leftCamEntity);

    // Right camera: orthographic top-down, bottom-right viewport, spot light layer
    // only — so this view is lit by the yellow spot and never by the directional.
    // The +X up vector is what turns the board square-on in the viewport.
    auto* rightCamEntity = new Entity();
    rightCamEntity->setEngine(engine.get());
    auto* rightCam = static_cast<CameraComponent*>(rightCamEntity->addComponent<CameraComponent>());
    if (rightCam && rightCam->camera()) {
        rightCam->setLayers({LAYERID_SPOTLIGHT, LAYERID_SKYBOX});
        rightCam->camera()->setRect(Vector4(0.5f, 0.0f, 0.5f, 0.5f));
        rightCam->camera()->setScissorRect(Vector4(0.5f, 0.0f, 0.5f, 0.5f));
        rightCam->camera()->setProjection(ProjectionType::Orthographic);
        rightCam->camera()->setOrthoHeight(150.0f);
        rightCam->camera()->setFarClip(500.0f);
        rightCam->setToneMapping(TONEMAP_ACES);
        // DEVIATION: upstream lets every camera clear its own viewport. Here the
        // second and third cameras must not clear, or they wipe the viewports drawn
        // before them.
        rightCam->camera()->setClearColorBuffer(false);
        rightCam->camera()->setClearDepthBuffer(false);
        rightCam->camera()->setClearStencilBuffer(false);
    }
    rightCamEntity->setLocalPosition(0.0f, 150.0f, 0.0f);
    rightCamEntity->lookAt(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));
    engine->root()->addChild(rightCamEntity);

    // Top camera: perspective, full-width upper half, World layer.
    auto* topCamEntity = new Entity();
    topCamEntity->setEngine(engine.get());
    auto* topCam = static_cast<CameraComponent*>(topCamEntity->addComponent<CameraComponent>());
    if (topCam && topCam->camera()) {
        topCam->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
        topCam->camera()->setRect(Vector4(0.0f, 0.5f, 1.0f, 0.5f));
        topCam->camera()->setScissorRect(Vector4(0.0f, 0.5f, 1.0f, 0.5f));
        topCam->camera()->setFarClip(500.0f);
        topCam->setToneMapping(TONEMAP_ACES);
        topCam->camera()->setClearColorBuffer(false);
        topCam->camera()->setClearDepthBuffer(false);
        topCam->camera()->setClearStencilBuffer(false);
    }
    topCamEntity->setLocalPosition(-100.0f, 75.0f, 100.0f);
    topCamEntity->lookAt(Vector3(0.0f, 7.0f, 0.0f));
    engine->root()->addChild(topCamEntity);

    // Guard against unintended extra cameras rendering full-screen.
    for (auto* cameraComp : CameraComponent::instances()) {
        if (!cameraComp) {
            continue;
        }
        if (cameraComp != leftCam && cameraComp != rightCam && cameraComp != topCam) {
            cameraComp->setEnabled(false);
            spdlog::warn("Disabled unintended camera component in multi-view example");
        }
    }

    auto logCameraRect = [](const char* name, CameraComponent* cameraComp) {
        if (!cameraComp || !cameraComp->camera()) {
            return;
        }
        const auto rect = cameraComp->camera()->rect();
        spdlog::info(
            "{} rect=({}, {}, {}, {})",
            name,
            rect.getX(),
            rect.getY(),
            rect.getZ(),
            rect.getW()
        );
    };
    logCameraRect("LeftCamera", leftCam);
    logCameraRect("RightCamera", rightCam);
    logCameraRect("TopCamera", topCam);

    if (composition) {
        const auto& actions = composition->renderActions();
        spdlog::info("Multi-view render actions: {}", actions.size());
        for (size_t i = 0; i < actions.size(); ++i) {
            const auto* action = actions[i];
            if (!action || !action->camera || !action->layer) {
                continue;
            }
            const char* cameraName = action->camera == leftCam
                ? "LeftCamera"
                : (action->camera == rightCam
                    ? "RightCamera"
                    : (action->camera == topCam ? "TopCamera" : "OtherCamera"));
            spdlog::info(
                "  [{}] {} layer={} transparent={} enabled={}",
                i,
                cameraName,
                action->layer->id(),
                action->transparent ? "true" : "false",
                action->camera->enabled() ? "true" : "false"
            );
        }
    }

    // Directional light affects only world-layer cameras.
    auto* dirLightEntity = new Entity();
    dirLightEntity->setEngine(engine.get());
    auto* dirLight = static_cast<LightComponent*>(dirLightEntity->addComponent<LightComponent>());
    if (dirLight) {
        dirLight->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        dirLight->setLayers({LAYERID_WORLD});
        dirLight->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        dirLight->setIntensity(5.0f);
        dirLight->setRange(500.0f);
        dirLight->setShadowDistance(500.0f);
        dirLight->setCastShadows(true);
        dirLight->setShadowBias(0.2f);
        dirLight->setShadowNormalBias(0.05f);
    }
    dirLightEntity->setLocalEulerAngles(45.0f, 0.0f, 30.0f);
    engine->root()->addChild(dirLightEntity);

    // Spot light affects only right-camera layer.
    auto* spotLightEntity = new Entity();
    spotLightEntity->setEngine(engine.get());
    auto* spotLight = static_cast<LightComponent*>(spotLightEntity->addComponent<LightComponent>());
    if (spotLight) {
        spotLight->setType(LightType::LIGHTTYPE_SPOT);
        spotLight->setLayers({LAYERID_SPOTLIGHT});
        spotLight->setColor(Color(1.0f, 1.0f, 0.0f, 1.0f));
        spotLight->setIntensity(7.0f);
        spotLight->setInnerConeAngle(20.0f);
        spotLight->setOuterConeAngle(80.0f);
        spotLight->setRange(200.0f);
        spotLight->setShadowDistance(200.0f);
        spotLight->setCastShadows(true);
        spotLight->setShadowBias(0.2f);
        spotLight->setShadowNormalBias(0.05f);
    }
    // Left unrotated on purpose, as upstream does: the beam points straight down
    // its own -Y and the light simply slides around above the board.
    spotLightEntity->setLocalPosition(40.0f, 60.0f, 40.0f);
    engine->root()->addChild(spotLightEntity);

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float time = 0.0f;

    // Debug shader passes on the top viewport, matching where upstream demonstrates them.
    // Switching costs no shader recompile — the mode is a runtime uniform.
    struct DebugPassEntry
    {
        DebugShaderPass pass;
        const char* name;
    };
    constexpr DebugPassEntry debugPasses[] = {
        {DebugShaderPass::DEBUGPASS_NONE, "none (forward)"},
        {DebugShaderPass::DEBUGPASS_ALBEDO, "albedo"},
        {DebugShaderPass::DEBUGPASS_WORLDNORMAL, "world normal"},
        {DebugShaderPass::DEBUGPASS_OPACITY, "opacity"},
        {DebugShaderPass::DEBUGPASS_SPECULARITY, "specularity"},
        {DebugShaderPass::DEBUGPASS_GLOSS, "gloss"},
        {DebugShaderPass::DEBUGPASS_METALNESS, "metalness"},
        {DebugShaderPass::DEBUGPASS_AO, "ao"},
        {DebugShaderPass::DEBUGPASS_EMISSION, "emission"},
        {DebugShaderPass::DEBUGPASS_LIGHTING, "lighting"},
        {DebugShaderPass::DEBUGPASS_UV0, "uv0"},
    };
    size_t debugPassIndex = 0;
    bool debugPassAutoCycle = true;
    float debugPassTimer = 0.0f;
    constexpr float debugPassInterval = 2.5f;

    const auto applyDebugPass = [&]() {
        // Upstream's HUD drives the top and right cameras together; the left one
        // stays on the forward pass as a reference.
        if (topCam && topCam->camera()) {
            topCam->camera()->setDebugShaderPass(debugPasses[debugPassIndex].pass);
        }
        if (rightCam && rightCam->camera()) {
            rightCam->camera()->setDebugShaderPass(debugPasses[debugPassIndex].pass);
        }
        spdlog::info("Top + right viewport debug pass: {}", debugPasses[debugPassIndex].name);
    };
    applyDebugPass();
    spdlog::info("Press D to step the top viewport's debug shader pass (stops auto-cycling)");

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_D) {
                debugPassAutoCycle = false;
                debugPassIndex = (debugPassIndex + 1) % std::size(debugPasses);
                applyDebugPass();
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq));
        prevCounter = nowCounter;
        time += dt;

        if (debugPassAutoCycle) {
            debugPassTimer += dt;
            if (debugPassTimer >= debugPassInterval) {
                debugPassTimer = 0.0f;
                debugPassIndex = (debugPassIndex + 1) % std::size(debugPasses);
                applyDebugPass();
            }
        }

        // Orbit the left camera, slide the spot light, and breathe the ortho view.
        leftCamEntity->setLocalPosition(100.0f * std::sin(time * 0.2f), 35.0f, 100.0f * std::cos(time * 0.2f));
        leftCamEntity->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        spotLightEntity->setLocalPosition(40.0f * std::sin(time * 0.5f), 60.0f, 40.0f * std::cos(time * 0.5f));

        if (rightCam && rightCam->camera()) {
            rightCam->camera()->setOrthoHeight(90.0f + std::sin(time * 0.3f) * 60.0f);
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();
    return 0;
}
