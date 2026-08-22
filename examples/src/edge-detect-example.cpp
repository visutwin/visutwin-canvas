// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Compute-shader edge detection: a chess board renders into an offscreen target
// through its own camera, an app-authored compute kernel runs a Sobel filter over
// that image, and two full-screen quad passes display the original above the
// edge-detected result.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/renderPassDownsample.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int LAYERID_RT = 70;

    constexpr const char* EDGE_DETECT_COMPUTE_SOURCE_METAL = R"(
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

    constexpr const char* EDGE_DETECT_COMPUTE_SOURCE_VULKAN = R"(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D inputTexture;
layout(rgba8, set = 0, binding = 1) uniform writeonly image2D outputTexture;
void main()
{
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputTexture);
    if (any(greaterThanEqual(gid, size))) return;
    vec2 uv = (vec2(gid) + 0.5) / vec2(size);
    vec2 texel = 1.0 / vec2(size);
    const vec3 weights = vec3(0.299, 0.587, 0.114);
    float tl = dot(texture(inputTexture, uv + texel * vec2(-1, -1)).rgb, weights);
    float tc = dot(texture(inputTexture, uv + texel * vec2( 0, -1)).rgb, weights);
    float tr = dot(texture(inputTexture, uv + texel * vec2( 1, -1)).rgb, weights);
    float ml = dot(texture(inputTexture, uv + texel * vec2(-1,  0)).rgb, weights);
    float mr = dot(texture(inputTexture, uv + texel * vec2( 1,  0)).rgb, weights);
    float bl = dot(texture(inputTexture, uv + texel * vec2(-1,  1)).rgb, weights);
    float bc = dot(texture(inputTexture, uv + texel * vec2( 0,  1)).rgb, weights);
    float br = dot(texture(inputTexture, uv + texel * vec2( 1,  1)).rgb, weights);
    float gx = -tl - 2.0 * ml - bl + tr + 2.0 * mr + br;
    float gy = -tl - 2.0 * tc - tr + bl + 2.0 * bc + br;
    float edge = length(vec2(gx, gy));
    vec4 src = texture(inputTexture, uv);
    imageStore(outputTexture, gid,
        vec4(mix(src.rgb, vec3(1, 0, 0), clamp(edge * 3.0, 0.0, 1.0)), 1));
}
)";

    // Mirrors upstream's instantiateRenderEntity({ castShadows, receiveShadows, layers }).
    void applyRenderOptionsRecursive(GraphNode* node, const std::vector<int>& layers)
    {
        if (!node) {
            return;
        }

        if (auto* entity = dynamic_cast<Entity*>(node)) {
            if (auto* render = entity->findComponent<RenderComponent>()) {
                render->setLayers(layers);
                render->setCastShadows(true);
                render->setReceiveShadows(true);
            }
        }

        for (const auto& child : node->children()) {
            applyRenderOptionsRecursive(child.get(), layers);
        }
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

        for (const auto& child : node->children()) {
            gatherRenderableStats(child.get(), stats);
        }
    }

}

class EdgeDetectExample final: public ExampleApp
{
public:
    EdgeDetectExample()
        : ExampleApp({.title = "Compute Edge Detect", .width = 1200, .height = 800}) {}

protected:
    bool create() override
    {
        auto composition = std::make_shared<LayerComposition>("edge-detect");
        auto defaultLayers = scene()->layers();
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
        scene()->setLayers(composition);

        _boardAsset = std::make_unique<Asset>(
            "board", AssetType::CONTAINER, assetPath("models/chess-board.glb"));
        _helipadAsset = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false}
        );

        const auto boardResource = _boardAsset->resource();
        const auto helipadResource = _helipadAsset->resource();
        if (!boardResource || !helipadResource || !std::holds_alternative<ContainerResource*>(*boardResource)) {
            spdlog::error("Failed to load required chess-board/env atlas resources");
            return false;
        }

        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));
        scene()->setSkyboxMip(1.0f);
        scene()->setSkyboxIntensity(1.0f);
        scene()->setExposure(1.0f);
        scene()->setToneMapping(TONEMAP_LINEAR);

        auto* container = std::get<ContainerResource*>(*boardResource);
        auto* boardEntity = container ? container->instantiateRenderEntity() : nullptr;
        if (!boardEntity) {
            spdlog::error("Failed to instantiate chess-board.glb render entity");
            return false;
        }
        boardEntity->setEngine(engine());
        applyRenderOptionsRecursive(boardEntity, {LAYERID_RT});
        root()->addChild(boardEntity);

        // The board keeps its authored transform, exactly like upstream. The model is
        // ~340 units across, so the orbiting render-target camera at radius 100 sits
        // among the pieces — that close-up is the shot the example is built around.
        // Re-scaling it to fit the frame turns it into a distant speck on white.

        RenderableStats boardStats;
        gatherRenderableStats(boardEntity, boardStats);
        if (boardStats.meshInstances == 0) {
            spdlog::error("chess-board.glb instantiated with zero mesh instances (renderComponents={}).",
                boardStats.renderComponents);
            spdlog::error("Draco is enabled in this build, so GLB decode likely failed for a different reason.");
            spdlog::error("Check parser warnings above for malformed Draco extension or decode errors.");
            return false;
        }

        // Directional light on the default WORLD layer, as upstream declares it. Note it
        // therefore does NOT reach the board, which lives on the RT layer only — the board
        // is lit purely by the environment atlas. Putting the light on LAYERID_RT would
        // add a key light upstream does not have.
        createDirectionalLight(Vector3(45.0f, 45.0f, 0.0f));

        const auto [initialW, initialH] = device()->size();
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
        _sourceTexture = std::make_shared<Texture>(device().get(), sourceTextureOptions);
        _sourceTexture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        _sourceTexture->setAddressV(ADDRESS_CLAMP_TO_EDGE);

        TextureOptions outputTextureOptions;
        outputTextureOptions.name = "EdgeDetectOutputStorage";
        outputTextureOptions.width = rtWidth;
        outputTextureOptions.height = rtHeight;
        outputTextureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        outputTextureOptions.mipmaps = false;
        outputTextureOptions.storage = true;
        outputTextureOptions.minFilter = FilterMode::FILTER_LINEAR;
        outputTextureOptions.magFilter = FilterMode::FILTER_LINEAR;
        _outputTexture = std::make_shared<Texture>(device().get(), outputTextureOptions);
        _outputTexture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        _outputTexture->setAddressV(ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions rtOptions;
        rtOptions.graphicsDevice = device().get();
        rtOptions.colorBuffer = _sourceTexture.get();
        rtOptions.depth = true;
        rtOptions.samples = 4;
        rtOptions.autoResolve = true;
        rtOptions.name = "EdgeDetectRT";
        _sceneRenderTarget = device()->createRenderTarget(rtOptions);

        _rtCameraEntity = createCamera(Vector3(100.0f, 35.0f, 0.0f));
        if (auto* rtCamera = _rtCameraEntity->findComponent<CameraComponent>();
            rtCamera && rtCamera->camera()) {
            rtCamera->setLayers({LAYERID_RT});
            rtCamera->camera()->setRenderTarget(_sceneRenderTarget);
            rtCamera->camera()->setFarClip(500.0f);
            rtCamera->camera()->setClearColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        }
        _rtCameraEntity->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        // Main camera: keeps its default layer set so the environment skybox fills the
        // background behind the two display quads (upstream relies on the same default).
        auto* mainCameraEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        if (auto* mainCamera = mainCameraEntity->findComponent<CameraComponent>();
            mainCamera && mainCamera->camera()) {
            mainCamera->camera()->setClearColor(Color(0.2f, 0.2f, 0.3f, 1.0f));
        }

        _displayOriginalPass = std::make_shared<RenderPassDownsample>(device(), _sourceTexture.get());
        _displayEdgePass = std::make_shared<RenderPassDownsample>(device(), _outputTexture.get());
        _displayOriginalPass->init(nullptr);
        _displayEdgePass->init(nullptr);
        _displayOriginalPass->setRequiresCubemaps(false);
        _displayEdgePass->setRequiresCubemaps(false);

        // Register the display passes as renderer append passes so they run inside
        // Engine::render(), after the scene render actions but BEFORE frame end.
        // Rendering to the back buffer after engine->render() returns is unsafe:
        // frameEnd presents the drawable, and a later pass would reuse the stale
        // presented drawable (pointer-auth SIGSEGV in startRenderPass).
        engine()->renderer()->addAppendPass(_displayOriginalPass);
        engine()->renderer()->addAppendPass(_displayEdgePass);

        if (device()->supportsCompute()) {
            ShaderDefinition computeDef;
            computeDef.name = "EdgeDetectCompute";
            computeDef.cshader = "edgeDetectKernel";
            // Both backends can be compiled in and selected at runtime (VISUTWIN_BACKEND),
            // so the source must be chosen from the live device, not from a build-time
            // #ifdef — that handed Metal the GLSL and failed every compile in a dual
            // backend build.
            const char* computeSource =
                device()->shaderLanguage() == ShaderLanguage::Glsl
                    ? EDGE_DETECT_COMPUTE_SOURCE_VULKAN
                    : EDGE_DETECT_COMPUTE_SOURCE_METAL;
            _computeShader = createShader(device().get(), computeDef, computeSource);
            if (_computeShader) {
                _compute = std::make_unique<Compute>(device().get(), _computeShader, "EdgeDetect");
                _compute->setParameter("inputTexture", _sourceTexture.get());
                _compute->setParameter("outputTexture", _outputTexture.get());
            }
        }

        return true;
    }

    void update(const float dt) override
    {
        _time += std::clamp(dt, 0.0f, 0.1f);

        const auto [w, h] = device()->size();
        const int desiredW = std::max(1, w);
        const int desiredH = std::max(1, h / 2);
        if (_sceneRenderTarget->width() != desiredW || _sceneRenderTarget->height() != desiredH) {
            _sceneRenderTarget->resize(desiredW, desiredH);
            _outputTexture->resize(static_cast<uint32_t>(desiredW), static_cast<uint32_t>(desiredH));
            if (_compute) {
                _compute->setParameter("inputTexture", _sourceTexture.get());
                _compute->setParameter("outputTexture", _outputTexture.get());
            }
        }

        const float cameraAngle = _time * 0.2f;
        _rtCameraEntity->setLocalPosition(100.0f * std::sin(cameraAngle), 35.0f, 100.0f * std::cos(cameraAngle));
        _rtCameraEntity->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        if (_compute) {
            const uint32_t dispatchX = static_cast<uint32_t>((_sceneRenderTarget->width() + 7) / 8);
            const uint32_t dispatchY = static_cast<uint32_t>((_sceneRenderTarget->height() + 7) / 8);
            _compute->setupDispatch(dispatchX, dispatchY, 1);
            device()->computeDispatch({_compute.get()}, "EdgeDetectDispatch");
        }

        // Two screen-space views with a small vertical gap — viewports set before
        // render(); the append passes draw them at the end of the frame graph.
        const float gap = 0.02f;
        const int screenW = std::max(1, w);
        const int screenH = std::max(1, h);
        const int vx = static_cast<int>(std::round(0.5f * gap * static_cast<float>(screenW)));
        const int vw = std::max(1, static_cast<int>(std::round((1.0f - gap) * static_cast<float>(screenW))));
        const int vh = std::max(1, static_cast<int>(std::round((0.5f - gap) * static_cast<float>(screenH))));
        const int topY = static_cast<int>(std::round(0.5f * gap * static_cast<float>(screenH)));
        const int bottomY = static_cast<int>(std::round((0.5f + 0.5f * gap) * static_cast<float>(screenH)));

        _displayOriginalPass->setViewport(Vector4(static_cast<float>(vx), static_cast<float>(topY),
            static_cast<float>(vw), static_cast<float>(vh)));
        _displayOriginalPass->setScissor(_displayOriginalPass->viewport());
        _displayEdgePass->setViewport(Vector4(static_cast<float>(vx), static_cast<float>(bottomY),
            static_cast<float>(vw), static_cast<float>(vh)));
        _displayEdgePass->setScissor(_displayEdgePass->viewport());
    }

    void destroy() override
    {
        // The append passes are registered with the renderer and the compute holds
        // device resources, so both go while the engine is still alive.
        _compute.reset();
        _computeShader.reset();
        _displayOriginalPass.reset();
        _displayEdgePass.reset();
        _sceneRenderTarget.reset();
        _sourceTexture.reset();
        _outputTexture.reset();
    }

private:
    std::unique_ptr<Asset> _boardAsset;
    std::unique_ptr<Asset> _helipadAsset;

    std::shared_ptr<Texture> _sourceTexture;
    std::shared_ptr<Texture> _outputTexture;
    std::shared_ptr<RenderTarget> _sceneRenderTarget;
    std::shared_ptr<RenderPassDownsample> _displayOriginalPass;
    std::shared_ptr<RenderPassDownsample> _displayEdgePass;
    std::shared_ptr<Shader> _computeShader;
    std::unique_ptr<Compute> _compute;

    Entity* _rtCameraEntity = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(EdgeDetectExample)
