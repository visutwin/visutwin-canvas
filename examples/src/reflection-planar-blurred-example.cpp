// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
// Demonstrates blurred planar reflections using per-pixel distance-dependent
// Poisson disk sampling.  A reflective ground plane mirrors the scene with
// controllable blur amount, intensity, Fresnel fade, and distance fade.
//
// two reflection cameras — color (slot 9) and depth (slot 10).
// The depth camera renders distance-from-plane per pixel, enabling sharp contact
// reflections and progressively blurry reflections for objects far from the ground.
//
// Controls:
//   Orbit: LMB drag, Wheel zoom
//   B / V: increase / decrease blur amount
//   I / O: increase / decrease intensity
//   F / G: increase / decrease fade strength
//   A / S: increase / decrease angle fade
//   H / J: increase / decrease height range
//   M:     toggle reflections on/off
//   R:     reset parameters to defaults
//   ESC:   quit
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/constants.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/components/animation/animationComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/composition/layerComposition.h"
#include "scene/materials/standardMaterial.h"
#include "core/math/matrix4.h"
#include "core/shape/boundingBox.h"

constexpr int WINDOW_WIDTH = 1200;
constexpr int WINDOW_HEIGHT = 800;

using namespace visutwin::canvas;

// Custom layer ID for the ground reflector (excluded from reflection camera).
constexpr int LAYERID_GROUND_REFLECTOR = 100;

SDL_Window* window;
SDL_Renderer* renderer;

const std::string rootPath = ASSET_DIR;

// Environment atlas for IBL.
const auto envAtlas = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

// GLB model to reflect.
const auto statueAsset = std::make_unique<Asset>(
    "statue",
    AssetType::CONTAINER,
    rootPath + "/models/antique_camera.glb"
);

/// Utility: compute AABB over all RenderComponent mesh instances under `entity`.
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
}

/// Utility: point camera at target using euler angles.
void setLookAt(Entity* camera, const Vector3& target)
{
    if (!camera) {
        return;
    }
    const Vector3 position = camera->position();
    const Vector3 lookDir = (target - position).normalized();
    const float pitchDeg = std::asin(std::clamp(lookDir.getY(), -1.0f, 1.0f)) * RAD_TO_DEG;
    const float yawDeg = std::atan2(-lookDir.getX(), -lookDir.getZ()) * RAD_TO_DEG;
    camera->setLocalEulerAngles(pitchDeg, yawDeg, 0.0f);
}

/// Helper to create a primitive entity with material and layers.
Entity* createPrimitiveEntity(
    Engine* engine, const std::string& type, const Vector3& position, const Vector3& scale,
    StandardMaterial* material, const std::vector<int>& layers = {LAYERID_WORLD})
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position);
    entity->setLocalScale(scale);

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setMaterial(material);
        render->setType(type);
        render->setLayers(layers);
    }

    engine->root()->addChild(entity);
    return entity;
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

    spdlog::info("*** VisuTwin Blurred Planar Reflection Example ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Blurred Planar Reflection",
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
    createOptions.registerComponentSystem<AnimationComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setSkyboxMip(0);
    scene->setSkyboxIntensity(2.0f);
    scene->setExposure(1.5f);
    scene->setToneMapping(TONEMAP_NEUTRAL);

    // Load environment atlas.
    const auto envAtlasResource = envAtlas->resource();
    if (!envAtlasResource) {
        spdlog::error("Failed to load environment atlas texture");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*envAtlasResource));

    // -----------------------------------------------------------------------
    // Layer composition: World + GroundReflector (excluded from reflection).
    // layer setup:
    //   const excludedLayer = new pc.Layer({ name: 'Excluded' });
    //   app.scene.layers.insertOpaque(excludedLayer, ...)
    // -----------------------------------------------------------------------
    auto groundLayer = std::make_shared<Layer>("GroundReflector", LAYERID_GROUND_REFLECTOR);
    scene->layers()->pushOpaque(groundLayer);
    scene->layers()->pushTransparent(groundLayer);

    // -----------------------------------------------------------------------
    // Scene objects — materials and primitives in the World layer.
    // -----------------------------------------------------------------------

    // Red metallic sphere.
    auto redMaterial = std::make_shared<StandardMaterial>();
    redMaterial->setDiffuse(Color(0.9f, 0.15f, 0.1f, 1.0f));
    redMaterial->setMetalness(0.6f);
    redMaterial->setGloss(0.8f);

    // Gold metallic box.
    auto goldMaterial = std::make_shared<StandardMaterial>();
    goldMaterial->setDiffuse(Color(1.0f, 0.84f, 0.0f, 1.0f));
    goldMaterial->setMetalness(0.9f);
    goldMaterial->setGloss(0.7f);

    // Blue dielectric cylinder.
    auto blueMaterial = std::make_shared<StandardMaterial>();
    blueMaterial->setDiffuse(Color(0.15f, 0.3f, 0.85f, 1.0f));
    blueMaterial->setMetalness(0.1f);
    blueMaterial->setGloss(0.9f);

    // White ceramic torus (cone as proxy).
    auto whiteMaterial = std::make_shared<StandardMaterial>();
    whiteMaterial->setDiffuse(Color(0.95f, 0.95f, 0.92f, 1.0f));
    whiteMaterial->setMetalness(0.0f);
    whiteMaterial->setGloss(0.85f);

    auto* sphere = createPrimitiveEntity(engine.get(), "sphere",
        Vector3(-1.5f, 1.0f, -0.5f), Vector3(2.0f, 2.0f, 2.0f), redMaterial.get());
    auto* box = createPrimitiveEntity(engine.get(), "box",
        Vector3(1.8f, 0.8f, -1.0f), Vector3(1.5f, 1.6f, 1.5f), goldMaterial.get());
    auto* cone = createPrimitiveEntity(engine.get(), "cone",
        Vector3(0.0f, 1.2f, 1.5f), Vector3(1.6f, 2.4f, 1.6f), blueMaterial.get());
    auto* cone2 = createPrimitiveEntity(engine.get(), "cone",
        Vector3(-3.0f, 0.6f, 2.0f), Vector3(1.0f, 1.2f, 1.0f), whiteMaterial.get());

    // Load GLB model if available.
    Entity* modelEntity = nullptr;
    const auto statueResource = statueAsset->resource();
    if (statueResource) {
        modelEntity = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
        if (modelEntity) {
            modelEntity->setLocalScale(0.5f, 0.5f, 0.5f);
            engine->root()->addChild(modelEntity);
        }
    }

    // -----------------------------------------------------------------------
    // Ground reflector — render component in GroundReflector layer only.
    // -----------------------------------------------------------------------
    auto groundMaterial = std::make_shared<StandardMaterial>();
    groundMaterial->setDiffuse(Color(0.85f, 0.85f, 0.85f, 1.0f));
    groundMaterial->setMetalness(0.0f);
    groundMaterial->setGloss(0.95f);   // High gloss for strong reflections.

    auto* groundEntity = createPrimitiveEntity(engine.get(), "plane",
        Vector3(0.0f, 0.0f, 0.0f), Vector3(20.0f, 1.0f, 20.0f),
        groundMaterial.get(), {LAYERID_GROUND_REFLECTOR});

    // -----------------------------------------------------------------------
    // Reflection depth render target and camera.
    // Blurred planar reflection:
    //   this._reflectionDepthCameraEntity = new Entity('ReflectionDepthCamera');
    //   reflectionDepthCamera.setShaderPass('planar_reflection_depth');
    //
    // The depth camera renders the scene (excluding ground) with a special
    // shader that outputs distance-from-reflection-plane as grayscale.
    // Created before color camera so it renders first.
    // -----------------------------------------------------------------------
    TextureOptions depthTexOpts;
    depthTexOpts.name = "ReflectionDepthRT";
    depthTexOpts.width = WINDOW_WIDTH;
    depthTexOpts.height = WINDOW_HEIGHT;
    depthTexOpts.format = PixelFormat::PIXELFORMAT_RGBA8;
    depthTexOpts.mipmaps = false;
    depthTexOpts.minFilter = FilterMode::FILTER_LINEAR;
    depthTexOpts.magFilter = FilterMode::FILTER_LINEAR;
    auto reflectionDepthTexture = std::make_shared<Texture>(graphicsDevice.get(), depthTexOpts);

    RenderTargetOptions depthRtOpts;
    depthRtOpts.graphicsDevice = graphicsDevice.get();
    depthRtOpts.colorBuffer = reflectionDepthTexture.get();
    depthRtOpts.depth = true;
    depthRtOpts.name = "ReflectionDepthRenderTarget";
    auto reflectionDepthRT = graphicsDevice->createRenderTarget(depthRtOpts);

    // Depth camera: renders World + Skybox with depth pass shader.
    auto* depthCamEntity = new Entity();
    depthCamEntity->setEngine(engine.get());
    auto* depthCamComp = static_cast<CameraComponent*>(depthCamEntity->addComponent<CameraComponent>());
    depthCamComp->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
    depthCamComp->camera()->setRenderTarget(reflectionDepthRT);
    depthCamComp->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));  // Black = zero distance
    depthCamComp->camera()->setPlanarReflectionDepthPass(true);  // Enable depth pass shader
    engine->root()->addChild(depthCamEntity);

    // -----------------------------------------------------------------------
    // Reflection color render target and camera.
    // -----------------------------------------------------------------------
    TextureOptions reflTexOpts;
    reflTexOpts.name = "ReflectionRT";
    reflTexOpts.width = WINDOW_WIDTH;
    reflTexOpts.height = WINDOW_HEIGHT;
    reflTexOpts.format = PixelFormat::PIXELFORMAT_RGBA8;
    reflTexOpts.mipmaps = false;
    reflTexOpts.minFilter = FilterMode::FILTER_LINEAR;
    reflTexOpts.magFilter = FilterMode::FILTER_LINEAR;
    auto reflectionTexture = std::make_shared<Texture>(graphicsDevice.get(), reflTexOpts);

    RenderTargetOptions reflRtOpts;
    reflRtOpts.graphicsDevice = graphicsDevice.get();
    reflRtOpts.colorBuffer = reflectionTexture.get();
    reflRtOpts.depth = true;
    reflRtOpts.name = "ReflectionRenderTarget";
    auto reflectionRT = graphicsDevice->createRenderTarget(reflRtOpts);

    // Reflection color camera: renders World + Skybox only (excludes ground layer).
    auto* reflCamEntity = new Entity();
    reflCamEntity->setEngine(engine.get());
    auto* reflCamComp = static_cast<CameraComponent*>(reflCamEntity->addComponent<CameraComponent>());
    reflCamComp->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
    reflCamComp->camera()->setRenderTarget(reflectionRT);
    reflCamComp->camera()->setClearColor(Color(0.5f, 0.5f, 0.5f, 1.0f));
    engine->root()->addChild(reflCamEntity);

    // -----------------------------------------------------------------------
    // Main camera with orbit controls.
    // -----------------------------------------------------------------------
    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    cameraEntity->addComponent<ScriptComponent>();

    if (cameraComp && cameraComp->camera()) {
        cameraComp->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX,
                               LAYERID_UI, LAYERID_IMMEDIATE, LAYERID_GROUND_REFLECTOR});
        cameraComp->camera()->setFov(60.0f);
        cameraComp->camera()->setNearClip(0.01f);
        cameraComp->camera()->setFarClip(200.0f);
        cameraComp->camera()->setClearColor(Color(0.7f, 0.7f, 0.75f, 1.0f));
    }

    cameraEntity->setLocalPosition(-2.0f, 3.5f, 8.0f);
    setLookAt(cameraEntity, Vector3(0.0f, 0.5f, 0.0f));
    engine->root()->addChild(cameraEntity);

    // Orbit camera controls.
    auto* cameraControls = cameraEntity->script()->create<CameraControls>();
    cameraControls->setFocusPoint(Vector3(0.0f, 0.5f, 0.0f));
    cameraControls->setEnableFly(false);
    cameraControls->setPitchRange(Vector2(-85.0f, -3.0f));  // Keep above ground.
    cameraControls->setOrbitDistance(10.0f);
    cameraControls->setAutoFarClip(true, 10.0f, 200.0f);
    cameraControls->storeResetState();

    // -----------------------------------------------------------------------
    // Directional light with shadows.
    // -----------------------------------------------------------------------
    auto* lightEntity = new Entity();
    lightEntity->setEngine(engine.get());
    auto* lightComp = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
    if (lightComp) {
        lightComp->setCastShadows(true);
        lightComp->setShadowResolution(2048);
        lightComp->setShadowDistance(50.0f);
        lightComp->setShadowBias(0.2f);
        lightComp->setIntensity(1.2f);
    }
    lightEntity->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
    engine->root()->addChild(lightEntity);

    // -----------------------------------------------------------------------
    // Apply reflection textures to ground material and graphics device.
    // -----------------------------------------------------------------------
    groundMaterial->setReflectionMap(reflectionTexture.get());
    graphicsDevice->setReflectionMap(reflectionTexture.get());
    graphicsDevice->setReflectionDepthMap(reflectionDepthTexture.get());

    // Initialize blur parameters (defaults).
    ReflectionBlurParams blurParams;
    blurParams.intensity = 1.0f;
    blurParams.blurAmount = 0.5f;
    blurParams.fadeStrength = 0.8f;
    blurParams.angleFade = 0.5f;
    blurParams.fadeColor = Color(0.5f, 0.5f, 0.5f, 1.0f);
    blurParams.planeDistance = 0.0f;   // Ground plane at Y = 0
    blurParams.heightRange = 10.0f;    // Normalize heights to 0..1 over 10 world units
    graphicsDevice->setReflectionBlurParams(blurParams);

    auto logBlurParams = [&]() {
        spdlog::info("Reflection: blur={:.2f} intensity={:.2f} fade={:.2f} angle={:.2f} height={:.1f}",
            blurParams.blurAmount, blurParams.intensity,
            blurParams.fadeStrength, blurParams.angleFade, blurParams.heightRange);
    };

    spdlog::info("Controls: B/V blur +/-, I/O intensity +/-, F/G fade +/-, A/S angle +/-, H/J height +/-, M toggle, R reset");
    logBlurParams();

    // -----------------------------------------------------------------------
    // Main loop.
    // -----------------------------------------------------------------------
    bool running = true;
    bool reflectionEnabled = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float time = 0.0f;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                const auto key = event.key.key;
                if (key == SDLK_ESCAPE) {
                    running = false;

                // Blur amount: B increase, V decrease.
                } else if (key == SDLK_B) {
                    blurParams.blurAmount = std::min(blurParams.blurAmount + 0.1f, 2.0f);
                    graphicsDevice->setReflectionBlurParams(blurParams);
                    logBlurParams();
                } else if (key == SDLK_V) {
                    blurParams.blurAmount = std::max(blurParams.blurAmount - 0.1f, 0.0f);
                    graphicsDevice->setReflectionBlurParams(blurParams);
                    logBlurParams();

                // Intensity: I increase, O decrease.
                } else if (key == SDLK_I) {
                    blurParams.intensity = std::min(blurParams.intensity + 0.1f, 1.0f);
                    graphicsDevice->setReflectionBlurParams(blurParams);
                    logBlurParams();
                } else if (key == SDLK_O) {
                    blurParams.intensity = std::max(blurParams.intensity - 0.1f, 0.0f);
                    graphicsDevice->setReflectionBlurParams(blurParams);
                    logBlurParams();

                // Fade strength: F increase, G decrease.
                } else if (key == SDLK_F) {
                    blurParams.fadeStrength = std::min(blurParams.fadeStrength + 0.2f, 5.0f);
                    graphicsDevice->setReflectionBlurParams(blurParams);
                    logBlurParams();
                } else if (key == SDLK_G) {
                    blurParams.fadeStrength = std::max(blurParams.fadeStrength - 0.2f, 0.1f);
                    graphicsDevice->setReflectionBlurParams(blurParams);
                    logBlurParams();

                // Angle fade: A increase, S decrease.
                } else if (key == SDLK_A) {
                    blurParams.angleFade = std::min(blurParams.angleFade + 0.1f, 1.0f);
                    graphicsDevice->setReflectionBlurParams(blurParams);
                    logBlurParams();
                } else if (key == SDLK_S) {
                    blurParams.angleFade = std::max(blurParams.angleFade - 0.1f, 0.1f);
                    graphicsDevice->setReflectionBlurParams(blurParams);
                    logBlurParams();

                // Height range: H increase, J decrease.
                } else if (key == SDLK_H) {
                    blurParams.heightRange = std::min(blurParams.heightRange + 1.0f, 50.0f);
                    graphicsDevice->setReflectionBlurParams(blurParams);
                    logBlurParams();
                } else if (key == SDLK_J) {
                    blurParams.heightRange = std::max(blurParams.heightRange - 1.0f, 1.0f);
                    graphicsDevice->setReflectionBlurParams(blurParams);
                    logBlurParams();

                // Reset to defaults.
                } else if (key == SDLK_R) {
                    blurParams.intensity = 1.0f;
                    blurParams.blurAmount = 0.5f;
                    blurParams.fadeStrength = 0.8f;
                    blurParams.angleFade = 0.5f;
                    blurParams.heightRange = 10.0f;
                    graphicsDevice->setReflectionBlurParams(blurParams);
                    cameraControls->reset();
                    spdlog::info("Parameters reset to defaults");
                    logBlurParams();

                // Toggle reflection on/off.
                } else if (key == SDLK_M) {
                    reflectionEnabled = !reflectionEnabled;
                    graphicsDevice->setReflectionMap(reflectionEnabled ? reflectionTexture.get() : nullptr);
                    graphicsDevice->setReflectionDepthMap(reflectionEnabled ? reflectionDepthTexture.get() : nullptr);
                    groundMaterial->setReflectionMap(reflectionEnabled ? reflectionTexture.get() : nullptr);
                    spdlog::info("Reflections {}", reflectionEnabled ? "ON" : "OFF");
                }
            }
        }

        const uint64_t currentCounter = SDL_GetPerformanceCounter();
        float deltaTime = static_cast<float>(currentCounter - prevCounter) / static_cast<float>(perfFreq);
        prevCounter = currentCounter;
        deltaTime = std::clamp(deltaTime, 0.0f, 0.1f);
        time += deltaTime;

        // Animate scene objects for visual interest.
        sphere->setLocalPosition(-1.5f, 1.0f + 0.3f * std::sin(time * 1.5f), -0.5f);
        box->setLocalEulerAngles(0.0f, time * 20.0f, 0.0f);
        cone->setLocalEulerAngles(0.0f, time * -15.0f, 0.0f);

        // -----------------------------------------------------------------------
        // Mirror main camera across ground plane for both reflection cameras.
        // Blurred planar reflection postUpdate():
        //   _reflectionMatrix.setReflection(plane.normal, plane.distance);
        //   reflectionMatrix.transformPoint(mainCameraPos, reflectedPos);
        // -----------------------------------------------------------------------
        if (reflectionEnabled) {
            constexpr float groundY = 0.0f;
            const float planeDistance = -groundY;  // d = -dot(normal, pointOnPlane)
            const Matrix4 reflMatrix = Matrix4::reflection(0.0f, 1.0f, 0.0f, planeDistance);

            const auto& camWorld = cameraEntity->worldTransform();
            const Vector3 camForward = Vector3(camWorld.getColumn(2)) * -1.0f;
            const Vector3 camPos = cameraEntity->position();
            const Vector3 camTarget = camPos + camForward;
            const Vector3 reflPos = reflMatrix.transformPoint(camPos);
            const Vector3 reflTarget = reflMatrix.transformPoint(camTarget);

            const Vector3 reflDir = (reflTarget - reflPos).normalized();
            const float pitch = std::asin(std::clamp(reflDir.getY(), -1.0f, 1.0f)) * RAD_TO_DEG;
            const float yaw = std::atan2(-reflDir.getX(), -reflDir.getZ()) * RAD_TO_DEG;

            // Update color reflection camera.
            reflCamEntity->setPosition(reflPos);
            reflCamEntity->setLocalEulerAngles(pitch, yaw, 0.0f);
            reflCamComp->camera()->setFov(cameraComp->camera()->fov());
            reflCamComp->camera()->setNearClip(cameraComp->camera()->nearClip());
            reflCamComp->camera()->setFarClip(cameraComp->camera()->farClip() * 2.0f);
            reflCamComp->camera()->setAspectRatio(cameraComp->camera()->aspectRatio());
            reflCamComp->camera()->setClearColor(blurParams.fadeColor);

            // Update depth reflection camera (identical transform).
            depthCamEntity->setPosition(reflPos);
            depthCamEntity->setLocalEulerAngles(pitch, yaw, 0.0f);
            depthCamComp->camera()->setFov(cameraComp->camera()->fov());
            depthCamComp->camera()->setNearClip(cameraComp->camera()->nearClip());
            depthCamComp->camera()->setFarClip(cameraComp->camera()->farClip() * 2.0f);
            depthCamComp->camera()->setAspectRatio(cameraComp->camera()->aspectRatio());
        }

        engine->update(deltaTime);
        engine->render();
    }

    // Cleanup: release reflection resources before engine destruction.
    graphicsDevice->setReflectionMap(nullptr);
    graphicsDevice->setReflectionDepthMap(nullptr);
    groundMaterial->setReflectionMap(nullptr);

    shutdown();
    return 0;
}
