// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream compute/indirect-dispatch.
//
// A compute shader decides how much work the next ones do. A camera renders the statue under a
// projected dome sky (the wide street HDRI, dome 200 units across, its centre 0.05 up, exposure
// 0.7) into a render target of the window's width and half its height, with a depth texture of
// its own. A scan kernel looks at that depth in 32 x 32 tiles, one thread a tile, linearises
// every fourth texel's depth and sorts the tile by its depth range into an EDGE list (a range over
// the threshold, 15 world units to begin with) or a SMOOTH list, counting both with atomics; the
// last tile to finish writes the two counts as the workgroup counts of two indirect dispatches,
// in two slots of the device's indirect dispatch buffer. Two effect kernels then run one 32 x 32
// workgroup per listed tile (Compute::setupIndirectDispatch), mixing the render half-way toward
// red on the edge tiles and toward blue on the smooth ones, into a storage texture. The render is
// shown in the top half of the window and the tinted result in the bottom half, over a camera
// clearing to (0.1, 0.1, 0.1). The scene camera (fov 70, near 0.1, far 500, ACES) starts at
// (-4, 5, 22) and orbits (0.17, 7.52, 0.02) under the mouse. The performance HUD shows the
// frame's compute dispatches and passes.
//
// Keys standing in for upstream's controls panel:
//   [ / ]  threshold -/+ 1 world unit (0.1 .. 50)
//
// DEVIATIONS:
// - The kernels are Slang, their bindings at the indices compute.h names (the buffers by name,
//   then the textures, then the uniform block). The depth is read as a depth texture on Metal and
//   through a combined sampler on Vulkan.
// - The three counters are cleared by a kernel of their own at the start of the frame's compute,
//   where upstream clears the buffers from the CPU.
// - The previews are TexturePreviewPasses (examples/texturePreview.h), upstream's TextureRenderer.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <algorithm>
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "../texturePreview.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/composition/layerComposition.h"
#include "scene/graphics/envLighting.h"
#include "scene/layer.h"
#include "scene/skybox/sky.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int LAYERID_RT = 20;
    constexpr uint32_t kTileSize = 32;
    constexpr float kCameraNear = 0.1f;
    constexpr float kCameraFar = 500.0f;

    // Clears the three counters, ahead of the scan.
    constexpr const char* kClearSlang = R"slang(
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> completionCounter : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> edgeTileCounter : register(u1);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> smoothTileCounter : register(u2);

[shader("compute")]
[numthreads(1, 1, 1)]
void clearMain(uint3 id : SV_DispatchThreadID)
{
    completionCounter[0] = 0u;
    edgeTileCounter[0] = 0u;
    smoothTileCounter[0] = 0u;
}
)slang";

    // Sorts the 32 x 32 tiles by their depth range into an edge and a smooth list, and writes the
    // two counts as indirect dispatch arguments.
    constexpr const char* kScanSlang = R"slang(
struct ScanParams
{
    float4 depth;   // x: depth range threshold, y: camera near, z: camera far
    uint4 tiles;    // x, y: tiles across and down, z: edge slot, w: smooth slot
};

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> completionCounter : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> edgeTileCounter : register(u1);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> edgeTileList : register(u2);
// Three uints a slot: workgroups x, y, z
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> indirectDispatchBuffer : register(u3);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> smoothTileCounter : register(u4);
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> smoothTileList : register(u5);
#if defined(VT_TARGET_METAL)
[[vk::binding(6, 0)]] DepthTexture2D depthTexture : register(t0);
#else
[[vk::binding(6, 0)]] [vk::image_format("unknown")] Sampler2D<float> depthTexture : register(t0);
#endif
[[vk::binding(7, 0)]] ConstantBuffer<ScanParams> params : register(b6);

static const uint TILE_SIZE = 32u;

// Linearize depth from [0,1] depth to view space depth
float linearizeDepth(float depth, float near, float far)
{
    return (near * far) / (far + depth * (near - far));
}

float loadDepth(int2 coord, uint2 texSize)
{
    const int2 clampedCoord = clamp(coord, int2(0, 0), int2(texSize) - int2(1, 1));
    return depthTexture.Load(int3(clampedCoord, 0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void scanMain(uint3 id : SV_DispatchThreadID)
{
    const uint tileX = id.x;
    const uint tileY = id.y;

    // Skip if outside tile grid
    if (tileX >= params.tiles.x || tileY >= params.tiles.y) {
        return;
    }

    uint2 texSize;
    depthTexture.GetDimensions(texSize.x, texSize.y);

    // Calculate tile bounds in pixels
    const uint startX = tileX * TILE_SIZE;
    const uint startY = tileY * TILE_SIZE;
    const uint endX = min(startX + TILE_SIZE, texSize.x);
    const uint endY = min(startY + TILE_SIZE, texSize.y);

    // Track min and max linearized depth in the tile, sampling every 4th pixel
    float minDepth = 1e10;
    float maxDepth = 0.0;
    for (uint y = startY; y < endY; y += 4u) {
        for (uint x = startX; x < endX; x += 4u) {
            const float linearDepth = linearizeDepth(loadDepth(int2(int(x), int(y)), texSize),
                params.depth.y, params.depth.z);
            minDepth = min(minDepth, linearDepth);
            maxDepth = max(maxDepth, linearDepth);
        }
    }

    // Classify tile based on whether its depth range exceeds the threshold
    const uint tileIndex = tileY * params.tiles.x + tileX;
    uint listIndex;
    if (maxDepth - minDepth > params.depth.x) {
        InterlockedAdd(edgeTileCounter[0], 1u, listIndex);
        edgeTileList[listIndex] = tileIndex;
    } else {
        InterlockedAdd(smoothTileCounter[0], 1u, listIndex);
        smoothTileList[listIndex] = tileIndex;
    }

    // The tile that completes the grid writes the dispatch arguments
    const uint totalTiles = params.tiles.x * params.tiles.y;
    uint completed;
    InterlockedAdd(completionCounter[0], 1u, completed);
    if (completed + 1u == totalTiles) {
        uint edgeCount;
        uint smoothCount;
        InterlockedAdd(edgeTileCounter[0], 0u, edgeCount);
        InterlockedAdd(smoothTileCounter[0], 0u, smoothCount);
        const uint edgeSlot = params.tiles.z * 3u;
        indirectDispatchBuffer[edgeSlot + 0u] = edgeCount;
        indirectDispatchBuffer[edgeSlot + 1u] = 1u;
        indirectDispatchBuffer[edgeSlot + 2u] = 1u;
        const uint smoothSlot = params.tiles.w * 3u;
        indirectDispatchBuffer[smoothSlot + 0u] = smoothCount;
        indirectDispatchBuffer[smoothSlot + 1u] = 1u;
        indirectDispatchBuffer[smoothSlot + 2u] = 1u;
    }
}
)slang";

    // One workgroup per listed tile: mixes the tile toward the tint colour.
    constexpr const char* kEffectSlang = R"slang(
struct EffectParams
{
    uint4 tiles;        // x, y: tiles across and down
    float4 tintColor;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> tileList : register(u0);
#if defined(VT_TARGET_METAL)
[[vk::binding(1, 0)]] Texture2D<float4> inputTexture : register(t0);
#else
[[vk::binding(1, 0)]] Sampler2D<float4> inputTexture : register(t0);
#endif
[[vk::binding(2, 0)]] [vk::image_format("rgba8")] RWTexture2D<float4> outputTexture : register(u1);
[[vk::binding(3, 0)]] ConstantBuffer<EffectParams> params : register(b1);

static const uint TILE_SIZE = 32u;

[shader("compute")]
[numthreads(32, 32, 1)]
void effectMain(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    // Each workgroup processes one tile: the workgroup's x is the index into the list
    const uint tileIndex = tileList[groupId.x];
    const uint tileX = tileIndex % params.tiles.x;
    const uint tileY = tileIndex / params.tiles.x;

    const uint2 pixel = uint2(tileX * TILE_SIZE + localId.x, tileY * TILE_SIZE + localId.y);
    uint2 texSize;
    outputTexture.GetDimensions(texSize.x, texSize.y);
    if (pixel.x >= texSize.x || pixel.y >= texSize.y) {
        return;
    }

    const float4 color = inputTexture.Load(int3(int2(pixel), 0));
    outputTexture[pixel] = float4(lerp(color.rgb, params.tintColor.rgb, 0.5), color.a);
}
)slang";

    struct ScanParams
    {
        float depth[4];
        uint32_t tiles[4];
    };

    struct EffectParams
    {
        uint32_t tiles[4];
        float tintColor[4];
    };

    std::shared_ptr<VertexBuffer> createStorageBuffer(GraphicsDevice* device, const int count)
    {
        VertexBufferOptions options;
        options.data.assign(static_cast<size_t>(count) * 4u, 0);
        return device->createVertexBuffer(std::make_shared<VertexFormat>(4, true, false), count, options);
    }

    void setRenderLayers(GraphNode* node, const std::vector<int>& layers)
    {
        if (auto* entity = dynamic_cast<Entity*>(node)) {
            if (auto* render = entity->findComponent<RenderComponent>()) {
                render->setLayers(layers);
            }
        }
        for (const auto& child : node->children()) {
            if (child) {
                setRenderLayers(child.get(), layers);
            }
        }
    }
}

class IndirectDispatchExample final : public ExampleApp
{
public:
    IndirectDispatchExample() : ExampleApp({.title = "Compute Indirect Dispatch", .miniStatsPresets = {"compute"}}) {}

protected:
    bool create() override
    {
        _statueAsset = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        _hdrAsset = std::make_unique<Asset>("hdri", AssetType::TEXTURE, assetPath("hdri/wide-street.hdr"),
            AssetData{.mipmaps = false});
        auto* statue = _statueAsset->resourceAs<ContainerResource>();
        Texture* hdr = _hdrAsset->resourceAs<Texture>();
        if (!statue || !hdr) {
            spdlog::error("indirect-dispatch needs models/statue.glb and hdri/wide-street.hdr");
            return false;
        }

        // A layer for the render target
        scene()->layers()->pushOpaque(std::make_shared<Layer>("RTLayer", LAYERID_RT));

        // The skydome from the HDR texture, projected onto a dome
        _skybox.reset(EnvLighting::generateSkyboxCubemap(device().get(), hdr));
        _envAtlas.reset(EnvLighting::generateAtlas(device().get(), hdr));
        scene()->setSkybox(_skybox.get());
        scene()->setEnvAtlas(_envAtlas.get());
        scene()->setSkyType(SKYTYPE_DOME);
        scene()->sky()->node()->setLocalScale(Vector3(200.0f, 200.0f, 200.0f));
        scene()->sky()->node()->setLocalPosition(Vector3(0.0f, 0.0f, 0.0f));
        scene()->sky()->setCenter(Vector3(0.0f, 0.05f, 0.0f));
        scene()->setExposure(0.7f);

        // The statue, on the render target's layer
        Entity* statueEntity = statue->instantiateRenderEntity();
        setRenderLayers(statueEntity, {LAYERID_RT});
        root()->addChild(statueEntity);

        // The camera rendering into the render target, with camera controls
        _rtCamera = createCamera(Vector3(-4.0f, 5.0f, 22.0f));
        _rtCamera->setName("rtCamera");
        auto* rtCamera = _rtCamera->findComponent<CameraComponent>();
        rtCamera->setLayers({LAYERID_RT, LAYERID_SKYBOX});
        rtCamera->setToneMapping(TONEMAP_ACES);
        rtCamera->setPriority(-1);
        rtCamera->camera()->setNearClip(kCameraNear);
        rtCamera->camera()->setFarClip(kCameraFar);
        rtCamera->camera()->setFov(70.0f);
        CameraControls* controls = addOrbitControls(_rtCamera, Vector3(0.17f, 7.52f, 0.02f));
        controls->setEnableFly(false);
        controls->setPitchRange(Vector2(-90.0f, 90.0f));
        controls->setZoomRange(Vector2(0.01f, 500.0f));

        // The main camera: only the texture previews, drawn after the frame
        Entity* mainCamera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        mainCamera->setName("mainCamera");
        auto* main = mainCamera->findComponent<CameraComponent>();
        main->setLayers({LAYERID_IMMEDIATE});
        main->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));

        // Top half: the render; bottom half: the compute-processed texture
        _sourcePreview = std::make_shared<TexturePreviewPass>(device(), nullptr);
        _outputPreview = std::make_shared<TexturePreviewPass>(device(), nullptr);
        for (const auto& preview : {_sourcePreview, _outputPreview}) {
            preview->init(nullptr);
            engine()->renderer()->addAppendPass(preview);
        }

        createResources();

        if (!device()->supportsCompute()) {
            spdlog::warn("indirect-dispatch: this device has no compute; only the render is shown");
            return true;
        }
        auto clearShader = getOrCreateSlangShaderFromSource(device().get(), "indirect-dispatch-clear", kClearSlang);
        auto scanShader = getOrCreateSlangShaderFromSource(device().get(), "indirect-dispatch-scan", kScanSlang);
        auto effectShader = getOrCreateSlangShaderFromSource(device().get(), "indirect-dispatch-effect", kEffectSlang);
        if (!clearShader || !scanShader || !effectShader) {
            spdlog::error("indirect-dispatch: a compute shader did not compile");
            return false;
        }
        _clearCompute = std::make_unique<Compute>(device().get(), clearShader, "ClearCounters");
        _scanCompute = std::make_unique<Compute>(device().get(), scanShader, "ScanCompute");
        _effectEdge = std::make_unique<Compute>(device().get(), effectShader, "EffectComputeEdge");
        _effectSmooth = std::make_unique<Compute>(device().get(), effectShader, "EffectComputeSmooth");

        spdlog::info("Keys: [ / ] threshold");
        return true;
    }

    void update(float /*dt*/) override
    {
        if (const auto* keyboard = engine()->keyboard()) {
            if (keyboard->wasPressed(Key::LeftBracket)) {
                _threshold = std::max(_threshold - 1.0f, 0.1f);
                spdlog::info("Threshold {:.1f}", _threshold);
            } else if (keyboard->wasPressed(Key::RightBracket)) {
                _threshold = std::min(_threshold + 1.0f, 50.0f);
                spdlog::info("Threshold {:.1f}", _threshold);
            }
        }

        // Recreate the resources when the window changes size
        const auto [width, height] = device()->size();
        if (width != _deviceWidth || height != _deviceHeight) {
            createResources();
        }

        // Display textures with a small gap between them
        constexpr float gap = 0.02f;
        placePreview(*_sourcePreview, gap * 0.5f, gap * 0.75f, 1.0f - gap, 0.5f - gap);
        placePreview(*_outputPreview, gap * 0.5f, 0.5f + gap * 0.25f, 1.0f - gap, 0.5f - gap);

        if (!_scanCompute) {
            return;
        }

        // Two slots of the indirect dispatch buffer for this frame
        const int edgeSlot = device()->getIndirectDispatchSlot();
        const int smoothSlot = device()->getIndirectDispatchSlot();
        if (edgeSlot < 0 || smoothSlot < 0) {
            return;
        }

        // Clear the counters
        _clearCompute->setParameter("completionCounter", _completionCounter);
        _clearCompute->setParameter("edgeTileCounter", _edgeTileCounter);
        _clearCompute->setParameter("smoothTileCounter", _smoothTileCounter);
        _clearCompute->setupDispatch(1u, 1u, 1u);

        // Pass 1: scan the tiles and classify them by depth discontinuity, a thread for each tile
        const ScanParams scan{{_threshold, kCameraNear, kCameraFar, 0.0f},
            {_numTilesX, _numTilesY, static_cast<uint32_t>(edgeSlot), static_cast<uint32_t>(smoothSlot)}};
        _scanCompute->setUniformBlock(&scan, sizeof(scan));
        _scanCompute->setParameter("depthTexture", _depthTexture.get());
        _scanCompute->setParameter("completionCounter", _completionCounter);
        _scanCompute->setParameter("edgeTileCounter", _edgeTileCounter);
        _scanCompute->setParameter("edgeTileList", _edgeTileList);
        _scanCompute->setParameter("smoothTileCounter", _smoothTileCounter);
        _scanCompute->setParameter("smoothTileList", _smoothTileList);
        _scanCompute->setParameter("indirectDispatchBuffer", device()->indirectDispatchBuffer());
        _scanCompute->setupDispatchThreads(_numTilesX, _numTilesY);
        device()->computeDispatch({_clearCompute.get(), _scanCompute.get()}, "ScanDispatch");

        // Pass 2: tint the edge tiles red (indirect dispatch)
        const EffectParams edge{{_numTilesX, _numTilesY, 0u, 0u}, {1.0f, 0.3f, 0.3f, 1.0f}};
        _effectEdge->setUniformBlock(&edge, sizeof(edge));
        _effectEdge->setParameter("tileList", _edgeTileList);
        _effectEdge->setParameter("inputTexture", _colorBuffer.get());
        _effectEdge->setParameter("outputTexture", _outputTexture.get());
        _effectEdge->setupIndirectDispatch(edgeSlot);
        device()->computeDispatch({_effectEdge.get()}, "EffectEdgeDispatch");

        // Pass 3: tint the smooth tiles blue (indirect dispatch)
        const EffectParams smooth{{_numTilesX, _numTilesY, 0u, 0u}, {0.3f, 0.3f, 1.0f, 1.0f}};
        _effectSmooth->setUniformBlock(&smooth, sizeof(smooth));
        _effectSmooth->setParameter("tileList", _smoothTileList);
        _effectSmooth->setParameter("inputTexture", _colorBuffer.get());
        _effectSmooth->setParameter("outputTexture", _outputTexture.get());
        _effectSmooth->setupIndirectDispatch(smoothSlot);
        device()->computeDispatch({_effectSmooth.get()}, "EffectSmoothDispatch");
    }

    void destroy() override
    {
        // The passes are registered with the renderer, so they go while the engine is alive.
        if (engine() && engine()->renderer()) {
            engine()->renderer()->removeAppendPass(_sourcePreview);
            engine()->renderer()->removeAppendPass(_outputPreview);
        }
        _sourcePreview.reset();
        _outputPreview.reset();
    }

private:
    // The render target (half the window's height), its depth texture, the output storage
    // texture and the tile lists, for the window's size.
    void createResources()
    {
        const auto [width, height] = device()->size();
        _deviceWidth = width;
        _deviceHeight = height;
        _rtWidth = static_cast<uint32_t>(std::max(width, 1));
        _rtHeight = static_cast<uint32_t>(std::max(height / 2, 1));
        _numTilesX = (_rtWidth + kTileSize - 1u) / kTileSize;
        _numTilesY = (_rtHeight + kTileSize - 1u) / kTileSize;
        const int numTiles = static_cast<int>(_numTilesX * _numTilesY);

        const auto createTexture = [this](const char* name, const PixelFormat format, const FilterMode filter,
            const bool storage) {
            TextureOptions options;
            options.name = name;
            options.width = _rtWidth;
            options.height = _rtHeight;
            options.format = format;
            options.mipmaps = false;
            options.minFilter = filter;
            options.magFilter = filter;
            options.storage = storage;
            auto texture = std::make_shared<Texture>(device().get(), options);
            texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
            texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
            return texture;
        };
        auto colorBuffer = createTexture("RT-ColorBuffer", PixelFormat::PIXELFORMAT_RGBA8, FilterMode::FILTER_LINEAR,
            false);
        auto depthTexture = createTexture("RT-DepthBuffer", PixelFormat::PIXELFORMAT_DEPTH, FilterMode::FILTER_NEAREST,
            false);
        auto outputTexture = createTexture("OutputTexture", PixelFormat::PIXELFORMAT_RGBA8, FilterMode::FILTER_LINEAR,
            true);

        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device().get();
        targetOptions.name = "SceneRT";
        targetOptions.colorBuffer = colorBuffer.get();
        targetOptions.depthBuffer = depthTexture.get();
        targetOptions.samples = 1;
        auto renderTarget = device()->createRenderTarget(targetOptions);

        // The camera takes the new target before the old one goes
        _rtCamera->findComponent<CameraComponent>()->camera()->setRenderTarget(renderTarget);
        _sourcePreview->setTexture(colorBuffer.get());
        _outputPreview->setTexture(outputTexture.get());
        _renderTarget = renderTarget;
        _colorBuffer = colorBuffer;
        _depthTexture = depthTexture;
        _outputTexture = outputTexture;

        // The tile lists and the counters
        _edgeTileList = createStorageBuffer(device().get(), numTiles);
        _smoothTileList = createStorageBuffer(device().get(), numTiles);
        _edgeTileCounter = createStorageBuffer(device().get(), 1);
        _smoothTileCounter = createStorageBuffer(device().get(), 1);
        _completionCounter = createStorageBuffer(device().get(), 1);
    }

    // A preview at (x, y) with the size (w, h), as fractions of the back buffer, from its top left.
    void placePreview(TexturePreviewPass& preview, const float x, const float y, const float w, const float h) const
    {
        const auto [width, height] = device()->size();
        const auto fw = static_cast<float>(width);
        const auto fh = static_cast<float>(height);
        preview.setViewport(Vector4(x * fw, y * fh, w * fw, h * fh));
        preview.setScissor(preview.viewport());
    }

    std::unique_ptr<Asset> _statueAsset;
    std::unique_ptr<Asset> _hdrAsset;
    std::unique_ptr<Texture> _skybox;
    std::unique_ptr<Texture> _envAtlas;
    Entity* _rtCamera = nullptr;

    std::shared_ptr<Texture> _colorBuffer;
    std::shared_ptr<Texture> _depthTexture;
    std::shared_ptr<Texture> _outputTexture;
    std::shared_ptr<RenderTarget> _renderTarget;
    std::shared_ptr<VertexBuffer> _edgeTileList;
    std::shared_ptr<VertexBuffer> _smoothTileList;
    std::shared_ptr<VertexBuffer> _edgeTileCounter;
    std::shared_ptr<VertexBuffer> _smoothTileCounter;
    std::shared_ptr<VertexBuffer> _completionCounter;

    std::unique_ptr<Compute> _clearCompute;
    std::unique_ptr<Compute> _scanCompute;
    std::unique_ptr<Compute> _effectEdge;
    std::unique_ptr<Compute> _effectSmooth;
    std::shared_ptr<TexturePreviewPass> _sourcePreview;
    std::shared_ptr<TexturePreviewPass> _outputPreview;

    int _deviceWidth = 0;
    int _deviceHeight = 0;
    uint32_t _rtWidth = 0;
    uint32_t _rtHeight = 0;
    uint32_t _numTilesX = 0;
    uint32_t _numTilesY = 0;
    float _threshold = 15.0f;
};

VISUTWIN_EXAMPLE_MAIN(IndirectDispatchExample)
