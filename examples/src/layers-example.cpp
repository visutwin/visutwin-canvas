// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream's graphics/layers: composes the frame from multiple layers.
// An X-Ray layer draws a character silhouette through walls using a greater depth
// test, a Character layer renders the walking character on top of it, and a Front
// layer with depth clearing keeps a held item from clipping into the scene.
// Keys 1-5 toggle the individual layers (upstream has UI checkboxes).
//
// Asset credit (apartment.glb): "Mirror's Edge Apartment - Interior Scene" by
// Aurelien Martel, CC BY-NC 4.0 — see assets/models/apartment.txt.
//
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#endif

#include <SDL3/SDL.h>
#ifdef VISUTWIN_HAS_METAL
#include <QuartzCore/QuartzCore.hpp>
#endif

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/animation/animationComponent.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/entity.h"
#include "framework/parsers/glbContainerResource.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1200;
constexpr int WINDOW_HEIGHT = 800;

// Layer ids for the three layers this example adds. Upstream allocates them
// automatically; here they just have to be unique and outside the built-in range.
constexpr int LAYERID_XRAY = 20;
constexpr int LAYERID_CHARACTER = 21;
constexpr int LAYERID_FRONT = 22;

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

const auto apartmentAsset = std::make_unique<Asset>(
    "apartment", AssetType::CONTAINER, rootPath + "/models/apartment.glb");
const auto bitmojiAsset = std::make_unique<Asset>(
    "bitmoji", AssetType::CONTAINER, rootPath + "/models/bitmoji.glb");
const auto walkAsset = std::make_unique<Asset>(
    "walk", AssetType::CONTAINER, rootPath + "/animations/bitmoji/walk.glb");
const auto cubeAsset = std::make_unique<Asset>(
    "cube", AssetType::CONTAINER, rootPath + "/models/playcanvas-cube.glb");

GlbContainerResource* loadContainer(const std::unique_ptr<Asset>& asset)
{
    if (!asset) {
        return nullptr;
    }
    const auto resource = asset->resource();
    if (!resource || !std::holds_alternative<ContainerResource*>(*resource)) {
        spdlog::error("GLB '{}' failed to load as a container", asset->name());
        return nullptr;
    }
    return dynamic_cast<GlbContainerResource*>(std::get<ContainerResource*>(*resource));
}

// Put every RenderComponent under an entity into the given layer.
void setEntityLayers(Entity* entity, const std::vector<int>& layers)
{
    if (!entity) {
        return;
    }
    for (auto* render : entity->findComponents<RenderComponent>()) {
        render->setLayers(layers);
    }
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

    spdlog::info("*** Layers Example ***");

#ifdef VISUTWIN_HAS_METAL
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
#endif
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Layers",
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
#ifdef VISUTWIN_HAS_VULKAN
        | SDL_WINDOW_VULKAN
#endif
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

    void* swapchain = nullptr;
#ifdef VISUTWIN_HAS_METAL
    swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        spdlog::error("Unable to get render Metal layer");
        shutdown();
        return -1;
    }
#endif

    GraphicsDeviceOptions deviceOptions;
#ifdef VISUTWIN_HAS_VULKAN
    deviceOptions.backend = Backend::Vulkan;
#endif
    deviceOptions.swapChain = swapchain;
    deviceOptions.window = window;
    auto device = createGraphicsDevice(deviceOptions);
    if (!device) {
        spdlog::error("Unable to create graphics device");
        shutdown();
        return -1;
    }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<ScriptComponentSystem>();
    createOptions.registerComponentSystem<AnimComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    // Setup skydome for environment lighting
    // JS: app.scene.envAtlas = assets.helipad.resource; app.scene.exposure = 1.2;
    auto scene = engine->scene();
    scene->setExposure(1.2f);

    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad texture");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    // ------ Layer setup ------

    const auto& layers = scene->layers();
    const auto worldLayer = layers->getLayerById(LAYERID_WORLD);
    if (!worldLayer) {
        spdlog::error("World layer missing from the default composition");
        shutdown();
        return -1;
    }

    // The 'X-Ray' layer renders after the World layer's opaque meshes. Meshes placed in it
    // draw on top of what the World layer rendered, which this example combines with a
    // greater depth test to show a character silhouette through walls. It is inserted before
    // the World layer's transparent sublayer, so that transparent meshes (which do not write
    // depth) still render on top of the silhouette.
    auto xrayLayer = std::make_shared<Layer>("X-Ray", LAYERID_XRAY);
    layers->insert(xrayLayer, layers->getTransparentIndex(worldLayer));

    // The 'Character' layer renders the character normally, after the X-Ray layer. Keeping
    // the character out of the World layer means the x-ray depth test only compares against
    // the world geometry, and rendering it after the X-Ray layer paints the visible parts of
    // the character over the silhouette.
    auto characterLayer = std::make_shared<Layer>("Character", LAYERID_CHARACTER);
    layers->insert(characterLayer, layers->getTransparentIndex(worldLayer));

    // The 'Front' layer renders last and clears the depth buffer before drawing, so its
    // meshes are never clipped by the world geometry - useful for held items or 3D HUD
    // elements.
    auto frontLayer = std::make_shared<Layer>("Front", LAYERID_FRONT);
    frontLayer->setClearDepthBuffer(true);
    layers->pushOpaque(frontLayer);
    layers->pushTransparent(frontLayer);

    // ------ Scene setup ------

    // Create an instance of the apartment and add it to the scene
    auto* apartmentContainer = loadContainer(apartmentAsset);
    if (!apartmentContainer) {
        shutdown();
        return -1;
    }
    auto* apartmentEntity = apartmentContainer->instantiateRenderEntity();
    apartmentEntity->setEngine(engine.get());
    engine->root()->addChild(apartmentEntity);
    apartmentEntity->setLocalScale(30.0f, 30.0f, 30.0f);

    // Add a concrete pillar between the camera and the walking path, to demonstrate the
    // x-ray effect when the character walks behind it
    auto pillarMaterial = std::make_shared<StandardMaterial>();
    pillarMaterial->setDiffuse(Color(0.58f, 0.57f, 0.55f));
    pillarMaterial->setGloss(0.3f);

    auto* pillar = new Entity();
    pillar->setName("Pillar");
    pillar->setEngine(engine.get());
    auto* pillarRender = static_cast<RenderComponent*>(pillar->addComponent<RenderComponent>());
    pillarRender->setMaterial(pillarMaterial.get());
    pillarRender->setType("box");
    engine->root()->addChild(pillar);
    pillar->setLocalScale(20.0f, 240.0f, 20.0f);
    pillar->setLocalPosition(-160.0f, 120.0f, -62.0f);

    // A root entity moved along a path each frame, carrying both copies of the walking character
    auto* walkerRoot = new Entity();
    walkerRoot->setName("WalkerRoot");
    walkerRoot->setEngine(engine.get());
    engine->root()->addChild(walkerRoot);

    // The character is rendered normally in the Character layer
    auto* bitmojiContainer = loadContainer(bitmojiAsset);
    auto* walkContainer = loadContainer(walkAsset);
    if (!bitmojiContainer || !walkContainer || walkContainer->animTracks().empty()) {
        spdlog::error("Character or walk animation failed to load");
        shutdown();
        return -1;
    }
    auto* walker = bitmojiContainer->instantiateRenderEntity();
    walker->setEngine(engine.get());
    walkerRoot->addChild(walker);
    walker->setLocalScale(60.0f, 60.0f, 60.0f);

    // JS: walker.addComponent('anim', { activate: true });
    //     walker.anim.assignAnimation('Walk', assets.walk.resource.animations[0].resource);
    // DEVIATION: the anim component here is state-graph driven, so the single looping clip
    // is expressed as a one-state graph rather than a bare assignAnimation call.
    if (auto* legacyAnim = walker->findComponent<AnimationComponent>()) {
        legacyAnim->setPlaying(false);
        legacyAnim->setEnabled(false);
    }
    {
        AnimStateGraph stateGraph;
        auto& animLayer = stateGraph.addLayer("locomotion");
        animLayer.states.push_back(AnimStateDesc{"Walk"});
        animLayer.transitions.push_back(AnimTransitionDesc{.from = "START", .to = "Walk"});

        auto* animComp = static_cast<AnimComponent*>(walker->addComponent<AnimComponent>());
        animComp->loadStateGraph(stateGraph);
        animComp->assignAnimation("Walk", walkContainer->animTracks().begin()->second);
    }

    setEntityLayers(walker, {LAYERID_CHARACTER});

    // Flat emissive material with a greater depth test - it only passes where the character
    // is behind already rendered geometry, so the silhouette shows only when occluded
    auto xrayMaterial = std::make_shared<StandardMaterial>();
    xrayMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f));
    xrayMaterial->setEmissive(Color(1.2f, 0.2f, 0.25f));
    {
        auto xrayDepth = std::make_shared<DepthState>();
        xrayDepth->setFunc(CompareFunction::Greater);
        xrayDepth->setDepthWrite(false);
        xrayMaterial->setDepthState(xrayDepth);
    }

    // Render the character a second time into the X-Ray layer, by adding mesh instances to
    // it directly. These share the mesh, node and skin instance with the normal copy, so
    // they render in the exact same pose, and the greater depth test only passes where the
    // character is hidden behind the world geometry.
    std::vector<MeshInstance*> xrayMeshInstances;
    for (auto* render : walker->findComponents<RenderComponent>()) {
        for (auto* meshInstance : render->meshInstances()) {
            if (!meshInstance || !meshInstance->mesh()) {
                continue;
            }
            auto* xrayInstance = new MeshInstance(
                meshInstance->mesh(), xrayMaterial.get(), meshInstance->node());
            xrayInstance->setSkinInstance(meshInstance->skinInstanceShared());
            xrayInstance->setMorphInstance(meshInstance->morphInstanceShared());
            xrayInstance->setCastShadow(false);
            xrayMeshInstances.push_back(xrayInstance);
        }
    }
    xrayLayer->addMeshInstances(xrayMeshInstances);
    spdlog::info("X-Ray layer: {} mesh instances sharing the character's pose", xrayMeshInstances.size());

    // Create an Entity with a camera component, rendering the default layers as well as the
    // three newly created layers
    auto* cameraEntity = new Entity();
    cameraEntity->setName("Camera");
    cameraEntity->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    cameraComp->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
    cameraComp->camera()->setFarClip(1500.0f);
    cameraComp->camera()->setFov(80.0f);
    cameraComp->setLayers({
        LAYERID_WORLD,
        LAYERID_DEPTH,
        LAYERID_SKYBOX,
        LAYERID_IMMEDIATE,
        LAYERID_UI,
        LAYERID_XRAY,
        LAYERID_CHARACTER,
        LAYERID_FRONT
    });
    engine->root()->addChild(cameraEntity);
    cameraEntity->setLocalPosition(-318.0f, 114.0f, -59.0f);

    // Add orbit camera controls, focused on the center of the walking path
    auto* script = static_cast<ScriptComponent*>(cameraEntity->addComponent<ScriptComponent>());
    auto* cameraControls = script ? script->create<CameraControls>() : nullptr;
    if (cameraControls) {
        // JS: cameraControls.focusPoint = new Vec3(-60, 45, -15);
        //     cameraControls.zoomRange = new Vec2(30, 600);
        cameraControls->setFocusPoint(Vector3(-60.0f, 45.0f, -15.0f));
        cameraControls->setZoomRange(Vector2(30.0f, 600.0f));
    }

    // Cube in the Front layer, attached to the camera like a held item. Because the Front
    // layer clears the depth buffer, the cube is never clipped by nearby walls.
    auto* cubeContainer = loadContainer(cubeAsset);
    if (!cubeContainer) {
        shutdown();
        return -1;
    }
    auto* cubeEntity = cubeContainer->instantiateRenderEntity();
    cubeEntity->setEngine(engine.get());
    cameraEntity->addChild(cubeEntity);
    setEntityLayers(cubeEntity, {LAYERID_FRONT});
    cubeEntity->setLocalScale(12.0f, 12.0f, 12.0f);
    cubeEntity->setLocalPosition(28.0f, -22.0f, -60.0f);

    // ------ UI handling ------
    // Upstream exposes five checkboxes; here they are keys.
    spdlog::info("Keys: 1 World  2 X-Ray  3 Character  4 Front  5 Front clear-depth  |  ESC quits");

    bool worldEnabled = true;
    bool xrayEnabled = true;
    bool characterEnabled = true;
    bool frontEnabled = true;
    bool frontClearDepth = true;

    // ------ Update loop ------

    const Vector3 walkCenter(-60.0f, 1.0f, -35.0f);
    constexpr float walkRadiusX = 90.0f;
    constexpr float walkRadiusZ = 50.0f;
    float angle = 0.0f;

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                switch (event.key.key) {
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_1:
                    worldEnabled = !worldEnabled;
                    worldLayer->setEnabled(worldEnabled);
                    layers->markDirty();
                    spdlog::info("World layer: {}", worldEnabled ? "on" : "off");
                    break;
                case SDLK_2:
                    xrayEnabled = !xrayEnabled;
                    xrayLayer->setEnabled(xrayEnabled);
                    layers->markDirty();
                    spdlog::info("X-Ray layer: {}", xrayEnabled ? "on" : "off");
                    break;
                case SDLK_3:
                    characterEnabled = !characterEnabled;
                    characterLayer->setEnabled(characterEnabled);
                    layers->markDirty();
                    spdlog::info("Character layer: {}", characterEnabled ? "on" : "off");
                    break;
                case SDLK_4:
                    frontEnabled = !frontEnabled;
                    frontLayer->setEnabled(frontEnabled);
                    layers->markDirty();
                    spdlog::info("Front layer: {}", frontEnabled ? "on" : "off");
                    break;
                case SDLK_5:
                    frontClearDepth = !frontClearDepth;
                    frontLayer->setClearDepthBuffer(frontClearDepth);
                    layers->markDirty();
                    spdlog::info("Front layer clear depth: {}", frontClearDepth ? "on" : "off");
                    break;
                default:
                    break;
                }
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;
        const float dt = static_cast<float>(dtSeconds);

        // walk in an ellipse on the open floor - the walk animation roughly matches this speed
        angle += dt * 0.6f;

        walkerRoot->setLocalPosition(
            walkCenter.getX() + std::sin(angle) * walkRadiusX,
            walkCenter.getY(),
            walkCenter.getZ() + std::cos(angle) * walkRadiusZ
        );

        // face the walking direction (tangent of the ellipse) - lookAt points the entity's -Z
        // axis at the target, and the model faces +Z, so look at the point behind the character
        walkerRoot->lookAt(Vector3(
            walkCenter.getX() + std::sin(angle - 0.1f) * walkRadiusX,
            walkCenter.getY(),
            walkCenter.getZ() + std::cos(angle - 0.1f) * walkRadiusZ
        ));

        // slowly spin the held cube
        cubeEntity->rotateLocal(0.0f, 20.0f * dt, 0.0f);

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** Layers Example Finished ***");

    return 0;
}
