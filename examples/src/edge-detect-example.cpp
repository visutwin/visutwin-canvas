// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/constants.h"
#include "framework/engine.h"
#include "framework/handlers/containerResource.h"
#include "log.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/renderPassDownsample.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int WINDOW_WIDTH = 1200;
    constexpr int WINDOW_HEIGHT = 800;
    constexpr int LAYERID_RT = 70;

    const std::string rootPath = ASSET_DIR;

    constexpr const char* EDGE_DETECT_COMPUTE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

kernel void edgeDetectKernel(
    texture2d<float, access::sample> inputTexture [[texture(0)]],
    texture2d<float, access::write> outputTexture [[texture(1)]],
    uint2 gid [[thread_position_in_grid]])
{
    const uint width = outputTexture.get_width();
    const uint height = outputTexture.get_height();
    if (gid.x >= width || gid.y >= height) {
        return;
    }

    auto luminance = [](float3 color) -> float {
        return dot(color, float3(0.299, 0.587, 0.114));
    };

    constexpr sampler linearClampSampler(coord::normalized, address::clamp_to_edge, filter::linear);
    const float2 texSize = float2(float(width), float(height));
    const float2 uv = (float2(gid) + 0.5) / texSize;
    const float2 texel = 1.0 / texSize;

    const float tl = luminance(inputTexture.sample(linearClampSampler, uv + texel * float2(-1.0, -1.0)).rgb);
    const float tc = luminance(inputTexture.sample(linearClampSampler, uv + texel * float2( 0.0, -1.0)).rgb);
    const float tr = luminance(inputTexture.sample(linearClampSampler, uv + texel * float2( 1.0, -1.0)).rgb);
    const float ml = luminance(inputTexture.sample(linearClampSampler, uv + texel * float2(-1.0,  0.0)).rgb);
    const float mr = luminance(inputTexture.sample(linearClampSampler, uv + texel * float2( 1.0,  0.0)).rgb);
    const float bl = luminance(inputTexture.sample(linearClampSampler, uv + texel * float2(-1.0,  1.0)).rgb);
    const float bc = luminance(inputTexture.sample(linearClampSampler, uv + texel * float2( 0.0,  1.0)).rgb);
    const float br = luminance(inputTexture.sample(linearClampSampler, uv + texel * float2( 1.0,  1.0)).rgb);

    const float gx = -tl - 2.0 * ml - bl + tr + 2.0 * mr + br;
    const float gy = -tl - 2.0 * tc - tr + bl + 2.0 * bc + br;
    const float edge = sqrt(gx * gx + gy * gy);

    const float4 src = inputTexture.sample(linearClampSampler, uv);
    const float edgeAmount = clamp(edge * 3.0, 0.0, 1.0);
    const float3 outColor = mix(src.rgb, float3(1.0, 0.0, 0.0), edgeAmount);
    outputTexture.write(float4(outColor, 1.0), gid);
}
)";

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

        for (auto* child : node->children()) {
            setRenderLayersRecursive(child, layers);
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

    struct RenderableStats
    {
        int renderComponents = 0;
        int meshInstances = 0;
    };

    void gatherRenderableStats(GraphNode* node, RenderableStats& stats)
    {
        if (!node) {
            return;
        }

        if (auto* entity = dynamic_cast<Entity*>(node)) {
            if (auto* render = entity->findComponent<RenderComponent>()) {
                stats.renderComponents++;
                stats.meshInstances += static_cast<int>(render->meshInstances().size());
            }
        }

        for (auto* child : node->children()) {
            gatherRenderableStats(child, stats);
        }
    }

}

int main()
{
    log::init();
    log::set_level_debug();

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window* window = SDL_CreateWindow(
        "VisuTwin Compute Edge Detect",
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        SDL_Quit();
        return -1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(
        createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window})
    );
    if (!graphicsDevice) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    AppOptions createOptions;
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

    auto composition = std::make_shared<LayerComposition>("edge-detect");
    auto defaultLayers = scene->layers();
    auto rtLayer = std::make_shared<Layer>("RTLayer", LAYERID_RT);
    composition->pushOpaque(rtLayer);
    if (defaultLayers) {
        if (auto layer = defaultLayers->getLayerById(LAYERID_WORLD)) {
            composition->pushOpaque(layer);
            composition->pushTransparent(layer);
        }
        if (auto layer = defaultLayers->getLayerById(LAYERID_DEPTH)) {
            composition->pushOpaque(layer);
        }
        if (auto layer = defaultLayers->getLayerById(LAYERID_SKYBOX)) {
            composition->pushOpaque(layer);
        }
        if (auto layer = defaultLayers->getLayerById(LAYERID_IMMEDIATE)) {
            composition->pushOpaque(layer);
            composition->pushTransparent(layer);
        }
        if (auto layer = defaultLayers->getLayerById(LAYERID_UI)) {
            composition->pushTransparent(layer);
        }
    }
    scene->setLayers(composition);

    auto boardAsset = std::make_unique<Asset>(
        "board",
        AssetType::CONTAINER,
        rootPath + "/models/a_beautiful_game.glb"
    );
    auto helipadAsset = std::make_unique<Asset>(
        "helipad-env-atlas",
        AssetType::TEXTURE,
        rootPath + "/cubemaps/helipad-env-atlas.png",
        AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false}
    );

    const auto boardResource = boardAsset->resource();
    const auto helipadResource = helipadAsset->resource();
    if (!boardResource || !helipadResource || !std::holds_alternative<ContainerResource*>(*boardResource)) {
        spdlog::error("Failed to load required chess-board/env atlas resources");
        engine.reset();
        graphicsDevice.reset();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));
    scene->setSkyboxMip(1.0f);
    scene->setSkyboxIntensity(1.0f);
    scene->setExposure(1.0f);
    scene->setToneMapping(TONEMAP_LINEAR);

    auto* container = std::get<ContainerResource*>(*boardResource);
    auto* boardEntity = container ? container->instantiateRenderEntity() : nullptr;
    if (!boardEntity) {
        spdlog::error("Failed to instantiate chess-board.glb render entity");
        engine.reset();
        graphicsDevice.reset();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }
    boardEntity->setEngine(engine.get());
    setRenderLayersRecursive(boardEntity, {LAYERID_RT});
    engine->root()->addChild(boardEntity);

    // Normalize board so longest extent is ~50 units, centered at origin
    {
        const auto bbox = calcEntityAABB(boardEntity);
        const auto& he = bbox.halfExtents();
        const auto& ct = bbox.center();
        const float maxExtent = std::max({he.getX(), he.getY(), he.getZ()}) * 2.0f;
        if (maxExtent > 0.001f) {
            const float s = 100.0f / maxExtent;
            boardEntity->setLocalScale(s, s, s);
            boardEntity->setLocalPosition(-ct.getX() * s, -ct.getY() * s, -ct.getZ() * s);
            spdlog::info("Board model: extent={:.1f}, scale={:.3f}", maxExtent, s);
        }
    }

    RenderableStats boardStats;
    gatherRenderableStats(boardEntity, boardStats);
    if (boardStats.meshInstances == 0) {
        spdlog::error("chess-board.glb instantiated with zero mesh instances (renderComponents={}).",
            boardStats.renderComponents);

        spdlog::error("Draco is enabled in this build, so GLB decode likely failed for a different reason.");
        spdlog::error("Check parser warnings above for malformed Draco extension or decode errors.");

        engine.reset();
        graphicsDevice.reset();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    auto* rtLight = new Entity();
    rtLight->setEngine(engine.get());
    auto* rtLightComp = static_cast<LightComponent*>(rtLight->addComponent<LightComponent>());
    if (rtLightComp) {
        rtLightComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        rtLightComp->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        rtLightComp->setIntensity(1.0f);
        rtLightComp->setLayers({LAYERID_RT});
    }
    rtLight->setLocalEulerAngles(45.0f, 45.0f, 0.0f);
    engine->root()->addChild(rtLight);

    const auto [initialW, initialH] = graphicsDevice->size();
    const int rtWidth = std::max(1, initialW);
    const int rtHeight = std::max(1, initialH / 2);

    TextureOptions sourceTextureOptions;
    sourceTextureOptions.name = "EdgeDetectSourceRT";
    sourceTextureOptions.width = rtWidth;
    sourceTextureOptions.height = rtHeight;
    sourceTextureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
    sourceTextureOptions.mipmaps = false;
    sourceTextureOptions.minFilter = FilterMode::FILTER_LINEAR;
    sourceTextureOptions.magFilter = FilterMode::FILTER_LINEAR;
    auto sourceTexture = std::make_shared<Texture>(graphicsDevice.get(), sourceTextureOptions);
    sourceTexture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
    sourceTexture->setAddressV(ADDRESS_CLAMP_TO_EDGE);

    TextureOptions outputTextureOptions;
    outputTextureOptions.name = "EdgeDetectOutputStorage";
    outputTextureOptions.width = rtWidth;
    outputTextureOptions.height = rtHeight;
    outputTextureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
    outputTextureOptions.mipmaps = false;
    outputTextureOptions.storage = true;
    outputTextureOptions.minFilter = FilterMode::FILTER_LINEAR;
    outputTextureOptions.magFilter = FilterMode::FILTER_LINEAR;
    auto outputTexture = std::make_shared<Texture>(graphicsDevice.get(), outputTextureOptions);
    outputTexture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
    outputTexture->setAddressV(ADDRESS_CLAMP_TO_EDGE);

    RenderTargetOptions rtOptions;
    rtOptions.graphicsDevice = graphicsDevice.get();
    rtOptions.colorBuffer = sourceTexture.get();
    rtOptions.depth = true;
    rtOptions.samples = 4;
    rtOptions.autoResolve = true;
    rtOptions.name = "EdgeDetectRT";
    auto sceneRenderTarget = graphicsDevice->createRenderTarget(rtOptions);

    auto* rtCameraEntity = new Entity();
    rtCameraEntity->setEngine(engine.get());
    auto* rtCamera = static_cast<CameraComponent*>(rtCameraEntity->addComponent<CameraComponent>());
    if (rtCamera && rtCamera->camera()) {
        rtCamera->setLayers({LAYERID_RT});
        rtCamera->camera()->setRenderTarget(sceneRenderTarget);
        rtCamera->camera()->setFarClip(500.0f);
        rtCamera->camera()->setClearColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
    }
    rtCameraEntity->setLocalPosition(60.0f, 30.0f, 60.0f);
    setLookAt(rtCameraEntity, Vector3(0.0f, 0.0f, 0.0f));
    engine->root()->addChild(rtCameraEntity);

    auto* mainCameraEntity = new Entity();
    mainCameraEntity->setEngine(engine.get());
    auto* mainCamera = static_cast<CameraComponent*>(mainCameraEntity->addComponent<CameraComponent>());
    if (mainCamera && mainCamera->camera()) {
        mainCamera->setLayers({LAYERID_WORLD});
        mainCamera->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        mainCamera->camera()->setNearClip(0.1f);
        mainCamera->camera()->setFarClip(200.0f);
        mainCamera->camera()->setProjection(ProjectionType::Perspective);
        mainCamera->camera()->setFov(60.0f);
    }
    mainCameraEntity->setLocalPosition(0.0f, 0.0f, 12.0f);
    setLookAt(mainCameraEntity, Vector3(0.0f, 0.0f, 0.0f));
    engine->root()->addChild(mainCameraEntity);

    auto displayOriginalPass = std::make_shared<RenderPassDownsample>(graphicsDevice, sourceTexture.get());
    auto displayEdgePass = std::make_shared<RenderPassDownsample>(graphicsDevice, outputTexture.get());
    displayOriginalPass->init(nullptr);
    displayEdgePass->init(nullptr);
    displayOriginalPass->setRequiresCubemaps(false);
    displayEdgePass->setRequiresCubemaps(false);

    std::shared_ptr<Shader> computeShader = nullptr;
    std::unique_ptr<Compute> compute = nullptr;
    if (graphicsDevice->supportsCompute()) {
        ShaderDefinition computeDef;
        computeDef.name = "EdgeDetectCompute";
        computeDef.cshader = "edgeDetectKernel";
        computeShader = createShader(graphicsDevice.get(), computeDef, EDGE_DETECT_COMPUTE_SOURCE);
        if (computeShader) {
            compute = std::make_unique<Compute>(graphicsDevice.get(), computeShader, "EdgeDetect");
            compute->setParameter("inputTexture", sourceTexture.get());
            compute->setParameter("outputTexture", outputTexture.get());
        }
    }

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float time = 0.0f;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }

        const uint64_t currentCounter = SDL_GetPerformanceCounter();
        float deltaTime = static_cast<float>(currentCounter - prevCounter) / static_cast<float>(perfFreq);
        prevCounter = currentCounter;
        deltaTime = std::clamp(deltaTime, 0.0f, 0.1f);
        time += deltaTime;

        const auto [w, h] = graphicsDevice->size();
        const int desiredW = std::max(1, w);
        const int desiredH = std::max(1, h / 2);
        if (sceneRenderTarget->width() != desiredW || sceneRenderTarget->height() != desiredH) {
            sceneRenderTarget->resize(desiredW, desiredH);
            outputTexture->resize(static_cast<uint32_t>(desiredW), static_cast<uint32_t>(desiredH));
            if (compute) {
                compute->setParameter("inputTexture", sourceTexture.get());
                compute->setParameter("outputTexture", outputTexture.get());
            }
        }

        const float cameraAngle = -time * 0.2f;
        rtCameraEntity->setLocalPosition(100.0f * std::sin(cameraAngle), 35.0f, 100.0f * std::cos(cameraAngle));
        setLookAt(rtCameraEntity, Vector3(0.0f, 0.0f, 0.0f));

        if (compute) {
            const uint32_t dispatchX = static_cast<uint32_t>((sceneRenderTarget->width() + 7) / 8);
            const uint32_t dispatchY = static_cast<uint32_t>((sceneRenderTarget->height() + 7) / 8);
            compute->setupDispatch(dispatchX, dispatchY, 1);
            graphicsDevice->computeDispatch({compute.get()}, "EdgeDetectDispatch");
        }

        engine->update(deltaTime);
        engine->render();

        // draw two screen-space textures with a small vertical gap.
        const float gap = 0.02f;
        const int screenW = std::max(1, w);
        const int screenH = std::max(1, h);
        const int vx = static_cast<int>(std::round(0.5f * gap * static_cast<float>(screenW)));
        const int vw = std::max(1, static_cast<int>(std::round((1.0f - gap) * static_cast<float>(screenW))));
        const int vh = std::max(1, static_cast<int>(std::round((0.5f - gap) * static_cast<float>(screenH))));
        const int topY = static_cast<int>(std::round(0.5f * gap * static_cast<float>(screenH)));
        const int bottomY = static_cast<int>(std::round((0.5f + 0.5f * gap) * static_cast<float>(screenH)));

        displayOriginalPass->viewport = Vector4(static_cast<float>(vx), static_cast<float>(topY), static_cast<float>(vw), static_cast<float>(vh));
        displayOriginalPass->scissor = displayOriginalPass->viewport;
        displayEdgePass->viewport = Vector4(static_cast<float>(vx), static_cast<float>(bottomY), static_cast<float>(vw), static_cast<float>(vh));
        displayEdgePass->scissor = displayEdgePass->viewport;

        displayOriginalPass->render();
        displayEdgePass->render();
    }

    engine.reset();
    graphicsDevice.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
