// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025
//
// MetalGraphicsDevice: construction, teardown, resource creation and capabilities. The
// frame, draw binding, render passes, compute, transfers and the bindless table live in
// metalGraphicsDevice<Component>.cpp.
#include <cmath>
#include "metalGraphicsDevice.h"
#include "platform/graphics/bindlessTable.h"

#include "platform/graphics/textureSamplerState.h"
#include "platform/graphics/runLoopWait.h"
#include <typeinfo>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ranges>
#include "metalTexture.h"
#include "core/scopedTimer.h"
#include "platform/graphics/screenshot.h"
#include "metalComputePipeline.h"
#include "metalIndexBuffer.h"
#include "metalShader.h"
#include "metalRenderPipeline.h"
#include "metalRenderTarget.h"
#include "metalUtils.h"
#include "metalVertexBuffer.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/stencilParameters.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    using metal::MetalBackBufferRenderTarget;

    namespace
    {
        // Per-draw uniform structs — defined at file scope so they can be used
        // by both setTransformUniforms() and the ring buffer sizing in the constructor.
        struct SceneData
        {
            simd::float4x4 projViewMatrix;
        };

        struct ModelData
        {
            simd::float4x4 modelMatrix;
            simd::float4x4 normalMatrix;
        };
    }

    size_t MetalGraphicsDevice::DepthStencilCacheKeyHash::operator()(
        const DepthStencilCacheKey& key) const noexcept
    {
        size_t hash = std::hash<uint32_t>{}(key.frontState);
        hash ^= std::hash<uint32_t>{}(key.backState) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
        hash ^= std::hash<bool>{}(key.depthTest) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
        hash ^= std::hash<bool>{}(key.depthWrite) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
        hash ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.depthFunc)) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
        return hash;
    }

    MetalGraphicsDevice::MetalGraphicsDevice(const GraphicsDeviceOptions& options):
        _window(options.window),
        _device(nullptr),
        _commandQueue(nullptr),
        _metalLayer(nullptr)
    {
        _renderPassEncoder = nullptr;

        _metalLayer = static_cast<CA::MetalLayer*>(options.swapChain);
        assert(_metalLayer && "Missing CAMetalLayer swap chain");

        _device = _metalLayer->device();
        assert(_device && "CAMetalLayer has no Metal device");

        _metalLayer->setDevice(_device);
        _metalLayer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        // DEVIATION: WebGL uses a persistent back buffer that survives across
        // render passes.  With Metal, multiple back-buffer passes in one frame (compose →
        // afterPass) need the drawable to be loadable across command buffers.  Setting
        // framebufferOnly to false allows LoadActionLoad to reliably load previous content.
        _metalLayer->setFramebufferOnly(false);
        // Frame pacing (options.vsync, default true): lock the render loop to
        // the display cadence so the wall-clock dt the app measures is even
        // (jittery dt shows as animation judder even at high fps).
        //  - displaySync must be forced ON: SDL's renderer (whose layer we
        //    borrow) may have disabled it, letting presents outrun the
        //    refresh rate entirely.
        //    That is also what stops the CPU bursting frames ahead and then
        //    stalling a whole vsync (dt alternating ~2 ms / ~33 ms): with sync
        //    on, three drawables pace exactly like two.
        //  - Three drawables, the layer's default, NOT two. In a fullscreen
        //    space the display holds one drawable through the next flip, so
        //    with only two the loop waits two intervals per frame: half the
        //    frame rate, and input lands late. Windowed mode, composited by
        //    the WindowServer, does not show it.
        // vsync=false leaves sync off and keeps three drawables for
        // maximum CPU/GPU overlap (uncapped-fps benchmarking).
        _metalLayer->setDisplaySyncEnabled(options.vsync);
        if (options.vsync) {
            _metalLayer->setMaximumDrawableCount(3);
        }

        _commandQueue = _device->newCommandQueue();
        assert(_commandQueue && "Failed to create Metal command queue");

        // Highest MSAA sample count this GPU accepts, used to clamp
        // RenderTargetOptions::samples. Left at the base-class default of 1 the clamp
        // would silently disable every multisampled target.
        for (const int candidate : {8, 4, 2}) {
            if (_device->supportsTextureSampleCount(static_cast<NS::UInteger>(candidate))) {
                setMaxSamples(candidate);
                break;
            }
        }

        // Metal publishes no runtime texture-size limit: the ceiling is a property
        // of the GPU family, and the feature-set tables give 16384 from Apple3 and
        // Mac2 onwards, 8192 below that. Everything that sizes a texture from
        // content — the lightmappers, the skybox cube bake, a shadow map — clamps
        // to this instead of to a literal 4096.
        const bool largeTextures = _device->supportsFamily(MTL::GPUFamilyApple3) ||
            _device->supportsFamily(MTL::GPUFamilyMac2);
        setMaxTextureSize(largeTextures ? 16384 : 8192);
        setMaxCubeMapSize(largeTextures ? 16384 : 8192);

        // Metal's sampler ratio is 1..16 on every family, so this is a constant
        // here rather than a query — but it is published so the sampler below and
        // Vulkan's per-texture samplers read ONE number. A hard-coded 16 on one side
        // and a queried device limit on the other agree on Apple hardware and would
        // diverge in silence anywhere else.
        setMaxAnisotropy(16.0f);

        // Metal publishes no limit on the threadgroups of a dispatch per dimension. MoltenVK
        // reports 2^30 for the same GPUs, so both backends spread a one-dimensional dispatch
        // over rows (Compute::setupDispatchThreads) at the same counts.
        setMaxComputeWorkgroupsPerDimension(1u << 30);

        // Half- and full-float colour attachments are renderable on every Metal
        // GPU this engine runs on; VSM shadows (RGBA16F moments) key on the first. So is
        // RG11B10Float, filtered, blended and multisampled, on every Apple GPU.
        setTextureHalfFloatRenderable(true);
        setTextureFloatRenderable(true);
        setTextureRG11B10Renderable(true);

        spdlog::info("Metal limits: {} samples, texture {}, cube {}, anisotropy {}",
            maxSamples(), maxTextureSize(), maxCubeMapSize(), maxAnisotropy());
        spdlog::info("Metal back buffer: {}x MSAA", resolveBackBufferSamples(options.antialias));

        // The default sampler: linear min and mag with trilinear mips (without a mip filter
        // Metal only ever samples mip 0 — aliasing and radial streaks at glancing angles),
        // repeat on S and T, and the device's anisotropy ratio, which keeps oblique ground
        // textures from smearing into radial lines. It is the sampler cache's entry for a
        // texture in its default state (defaultTextureSamplerState), and the cache maps
        // every texture's state the way the Vulkan backend's per-texture samplers do, so a
        // material map filters and wraps identically on both.
        _samplerCache.init(_device, maxAnisotropy());
        _defaultSampler = _samplerCache.defaultSampler();

        // Screen-space post passes (TAA/SSAO/CoC/DOF/blur/compose) must NOT use
        // the repeat-mode scene sampler: their kernels tap past [0,1] at frame
        // borders and repeat wraps to the opposite edge (wrong occlusion/color
        // at screen edges). No anisotropy: these are screen-aligned quads.
        //
        // The mip filter IS linear, as Vulkan's quad path samples through each texture's
        // own sampler: a single-mip render target is unaffected, but a pass that reads a
        // mip chain needs it. Metal's default (not mipmapped) samples level 0 whatever
        // level() asks for, so the environment bake's Lambert convolution — 2048 taps at
        // LOD 4.5 and up — read the full-resolution cube and the atlas's ambient rect came
        // out noise that matched Vulkan's at a correlation of 0.35.
        auto* postDesc = MTL::SamplerDescriptor::alloc()->init();
        postDesc->setMinFilter(MTL::SamplerMinMagFilterLinear);
        postDesc->setMagFilter(MTL::SamplerMinMagFilterLinear);
        postDesc->setMipFilter(MTL::SamplerMipFilterLinear);
        postDesc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
        postDesc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
        _postSampler = _device->newSamplerState(postDesc);
        postDesc->release();

        // The other two samplers every quad draw gets (sampler slots 1 and 2), for the Slang
        // programs, which cannot declare a constexpr sampler: nearest, clamp, no mips —
        // the point sampler a depth tap needs, the twin of the nearest sampler the Vulkan
        // quad path binds for every depth texture — and a linear less-equal comparison
        // sampler, clamp, for a hardware shadow compare.
        auto* pointDesc = MTL::SamplerDescriptor::alloc()->init();
        pointDesc->setMinFilter(MTL::SamplerMinMagFilterNearest);
        pointDesc->setMagFilter(MTL::SamplerMinMagFilterNearest);
        pointDesc->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
        pointDesc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
        pointDesc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
        _quadPointSampler = _device->newSamplerState(pointDesc);
        pointDesc->release();
        auto* compareDesc = MTL::SamplerDescriptor::alloc()->init();
        compareDesc->setMinFilter(MTL::SamplerMinMagFilterLinear);
        compareDesc->setMagFilter(MTL::SamplerMinMagFilterLinear);
        compareDesc->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
        compareDesc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
        compareDesc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
        compareDesc->setCompareFunction(MTL::CompareFunctionLessEqual);
        _quadCompareSampler = _device->newSamplerState(compareDesc);
        compareDesc->release();

        // Room for one clustered light record and a cell row; nothing reads them (see the
        // header), the size only has to be a valid buffer.
        constexpr size_t kClusterEmptyBytes = 4096;
        for (MTL::Buffer** buffer : {&_clusterEmptyLightBuffer, &_clusterEmptyCellBuffer}) {
            *buffer = _device->newBuffer(kClusterEmptyBytes, MTL::ResourceStorageModeShared);
            std::memset((*buffer)->contents(), 0, kClusterEmptyBytes);
        }

        auto* depthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        // Depth state is dynamic in Metal and is not part of the render PSO. Keep
        // all four DepthState test/write combinations ready so draw() can honor
        // both flags. LessEqual is required for the skybox at cleared depth 1.0.
        depthDesc->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        depthDesc->setDepthWriteEnabled(true);
        _defaultDepthStencilState = _device->newDepthStencilState(depthDesc);

        depthDesc->setDepthWriteEnabled(false);
        _noWriteDepthStencilState = _device->newDepthStencilState(depthDesc);

        depthDesc->setDepthCompareFunction(MTL::CompareFunctionAlways);
        depthDesc->setDepthWriteEnabled(true);
        _noTestDepthStencilState = _device->newDepthStencilState(depthDesc);

        depthDesc->setDepthWriteEnabled(false);
        _noTestNoWriteDepthStencilState = _device->newDepthStencilState(depthDesc);
        depthDesc->release();

        _renderPipeline = std::make_unique<MetalRenderPipeline>(this);
        _computePipeline = std::make_unique<MetalComputePipeline>(this);

        // Triple-buffered ring buffers for per-draw uniform data, one slot per allocation:
        // _transformRing takes ModelData (slot 2) once per draw, _uniformRing the material
        // block (slot 3) when it changes and the lighting block (slot 4) when THAT changes,
        // in slots sized for the lighting block. Both start small and double inside the
        // frame that needs more, so a scene pays for what it draws (sized for 4096 draws up
        // front, the two held 79 MB whatever the scene).
        static constexpr size_t kInitialDrawsPerFrame = 256;
        _transformRing = std::make_unique<MetalUniformRingBuffer>(
            _device, kInitialDrawsPerFrame, sizeof(ModelData), "TransformRing");
        _uniformRing = std::make_unique<MetalUniformRingBuffer>(
            _device, kInitialDrawsPerFrame, sizeof(MetalUniformBinder::LightingUniforms), "UniformRing");

        // Variable-size bump-allocator ring buffer for matrix palettes (dynamic batches
        // and skins). Starts at 256KB a frame and grows with what a frame asks for.
        _paletteRing = std::make_unique<MetalPaletteRingBuffer>(_device, _frameGate, "PaletteRing");

        // GPU pass profiler (MTLCounterSampleBuffer timestamps). Created when the
        // device supports stage-boundary sampling; disabled until setEnabled(true).
        _metalGpuProfiler = gpu::MetalGpuProfiler::create(_device);
        _gpuProfiler = _metalGpuProfiler;

        RenderTargetOptions backBufferOptions;
        backBufferOptions.graphicsDevice = this;
        backBufferOptions.name = "MetalBackBuffer";
        backBufferOptions.samples = samples();
        setBackBuffer(std::make_shared<MetalBackBufferRenderTarget>(backBufferOptions));
    }

    MetalGraphicsDevice::~MetalGraphicsDevice()
    {
        if (_preparedDrawable) {
            _preparedDrawable->release();
            _preparedDrawable = nullptr;
        }
        dropReadbacks();
        if (_renderPassEncoder) {
            if (_encoderDebugGroupOpen) { _renderPassEncoder->popDebugGroup(); _encoderDebugGroupOpen = false; }
            _renderPassEncoder->endEncoding();
            _renderPassEncoder = nullptr;
        }

        // Release what the base class holds (the current render target among it) while
        // this device and the base's own members still exist, as the Vulkan device does.
        // Left to the base's member destructors, the last render target an offline bake
        // bound is freed AFTER the target registry it erases itself from, and while the
        // shared_ptr releasing it is the one its destructor resets: a trap at teardown.
        releaseGpuReferences();

        // _pipelineState is a non-owning pointer; the render pipeline cache
        // owns pipeline states and releases them in its destructor.
        _pipelineState = nullptr;

        releaseBindlessTable();
        // Borrowed from the sampler cache, which releases it with the rest.
        _defaultSampler = nullptr;
        _samplerCache.release();

        if (_postSampler) {
            _postSampler->release();
            _postSampler = nullptr;
        }
        if (_quadPointSampler) {
            _quadPointSampler->release();
            _quadPointSampler = nullptr;
        }
        if (_quadCompareSampler) {
            _quadCompareSampler->release();
            _quadCompareSampler = nullptr;
        }

        if (_defaultDepthStencilState) {
            _defaultDepthStencilState->release();
            _defaultDepthStencilState = nullptr;
        }
        if (_noWriteDepthStencilState) {
            _noWriteDepthStencilState->release();
            _noWriteDepthStencilState = nullptr;
        }
        if (_noTestDepthStencilState) {
            _noTestDepthStencilState->release();
            _noTestDepthStencilState = nullptr;
        }
        if (_noTestNoWriteDepthStencilState) {
            _noTestNoWriteDepthStencilState->release();
            _noTestNoWriteDepthStencilState = nullptr;
        }
        for (auto& [_, state] : _stencilStateCache) {
            if (state) {
                state->release();
            }
        }
        _stencilStateCache.clear();

        for (MTL::Texture** texture : {&_backBufferMsaaColor, &_backBufferDepthResolve}) {
            if (*texture) {
                (*texture)->release();
                *texture = nullptr;
            }
        }
        if (_backBufferDepthTexture) {
            _backBufferDepthTexture->release();
            _backBufferDepthTexture = nullptr;
        }

        for (auto& slot : _clusterBufferSets) {
            for (auto& set : slot) {
                if (set.light) {
                    set.light->release();
                }
                if (set.cell) {
                    set.cell->release();
                }
            }
            slot.clear();
        }
        for (MTL::Buffer** buffer : {&_clusterEmptyLightBuffer, &_clusterEmptyCellBuffer}) {
            if (*buffer) {
                (*buffer)->release();
                *buffer = nullptr;
            }
        }
        _clusterLightBuffer = nullptr;
        _clusterCellBuffer = nullptr;


        // Whatever was encoded and never reached a frame end still runs.
        flushCommands();

        if (_framePool) {
            _framePool->release();
            _framePool = nullptr;
        }

        if (_commandQueue) {
            _commandQueue->release();
            _commandQueue = nullptr;
        }

        if (_device && _ownsDevice) {
            _device->release();
            _device = nullptr;
        }
    }

    std::pair<int, int> MetalGraphicsDevice::size() const
    {
        // When constructed without an SDL_Window (e.g. hosted inside an
        // AppKit/MTKView by an external app), the CAMetalLayer is the
        // authoritative source of drawable size. SDL_GetWindowSizeInPixels
        // would UB on a null window.
        if (!_window) {
            const CGSize s = _metalLayer->drawableSize();
            return {static_cast<int>(s.width), static_cast<int>(s.height)};
        }
        int w, h;
        SDL_GetWindowSizeInPixels(_window, &w, &h);
        // Capped below the display's density (maxPixelRatio), the drawable is the window in
        // points at the capped ratio, and the layer scales it up to the window.
        const float ratio = pixelRatio();
        if (ratio < devicePixelRatio()) {
            const auto [pw, ph] = windowSizeInPoints();
            return {static_cast<int>(std::floor(static_cast<float>(pw) * ratio)),
                    static_cast<int>(std::floor(static_cast<float>(ph) * ratio))};
        }
        return {w, h};
    }

    float MetalGraphicsDevice::devicePixelRatio() const
    {
        const float density = _window ? SDL_GetWindowPixelDensity(_window) : 0.0f;
        return density > 0.0f ? density : 1.0f;
    }

    std::pair<int, int> MetalGraphicsDevice::windowSizeInPoints() const
    {
        int w = 0, h = 0;
        if (_window) {
            SDL_GetWindowSize(_window, &w, &h);
        }
        return {w, h};
    }


    std::shared_ptr<Shader> MetalGraphicsDevice::createShader(const ShaderDefinition& definition,
        const std::string& sourceCode)
    {
        return std::make_shared<MetalShader>(this, definition, sourceCode);
    }

    std::shared_ptr<Shader> MetalGraphicsDevice::createShaderFromCode(const ShaderDefinition& definition,
        const ShaderCode& code)
    {
        std::shared_ptr<MetalShader> shader;
        if (!code.metalLibrary.empty()) {
            shader = std::make_shared<MetalShader>(this, definition, code.metalLibrary);
        } else if (!code.metalSource.empty()) {
            shader = std::make_shared<MetalShader>(this, definition, code.metalSource);
        } else {
            spdlog::error("MetalGraphicsDevice::createShaderFromCode('{}'): no MSL source and no metallib",
                definition.name);
            return nullptr;
        }
        shader->setSpecializeFeatures(code.specializeFeatures);
        // An MSL kernel declares no workgroup size; the dispatch supplies it, so the one the
        // Slang source declared comes with the code.
        shader->setComputeThreadGroupSize(code.computeThreadGroupSize);
        return shader;
    }

    std::unique_ptr<gpu::HardwareTexture> MetalGraphicsDevice::createGPUTexture(Texture* texture)
    {
        auto hwTexture = std::make_unique<gpu::MetalTexture>(texture);
        hwTexture->create(this);
        return hwTexture;
    }

    std::shared_ptr<VertexBuffer> MetalGraphicsDevice::createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
        const int numVertices, const VertexBufferOptions& options)
    {
        return std::make_shared<MetalVertexBuffer>(this, format, numVertices, options);
    }

    std::shared_ptr<VertexBuffer> MetalGraphicsDevice::createVertexBufferFromMTLBuffer(
        const std::shared_ptr<VertexFormat>& format,
        int numVertices, MTL::Buffer* externalBuffer)
    {
        return std::make_shared<MetalVertexBuffer>(this, format, numVertices, externalBuffer);
    }

    std::shared_ptr<VertexBuffer> MetalGraphicsDevice::createVertexBufferFromNativeBuffer(
        const std::shared_ptr<VertexFormat>& format,
        int numVertices, void* nativeBuffer)
    {
        return createVertexBufferFromMTLBuffer(format, numVertices, static_cast<MTL::Buffer*>(nativeBuffer));
    }


    std::shared_ptr<IndexBuffer> MetalGraphicsDevice::createIndexBuffer(const IndexFormat format, const int numIndices,
        const std::vector<uint8_t>& data)
    {
        auto indexBuffer = std::make_shared<MetalIndexBuffer>(this, format, numIndices);
        if (!data.empty()) {
            indexBuffer->setData(data);
        }
        return indexBuffer;
    }

    std::shared_ptr<RenderTarget> MetalGraphicsDevice::createRenderTarget(const RenderTargetOptions& options)
    {
        const ScopedMilliseconds timer(renderTargetCreationTimeTotal());   // stats.misc
        return std::make_shared<MetalRenderTarget>(options);
    }


    bool MetalGraphicsDevice::beginGpuCapture(const std::string& path)
    {
        if (!_device || _gpuCaptureActive) {
            return false;
        }
        auto* manager = MTL::CaptureManager::sharedCaptureManager();
        if (!manager->supportsDestination(MTL::CaptureDestinationGPUTraceDocument)) {
            spdlog::error("GPU capture: Metal writes no GPU trace document; set MTL_CAPTURE_ENABLED=1 before "
                "the device is created");
            return false;
        }
        // Work encoded before the capture belongs to another frame: submitted now, it stays out.
        flushCommands();
        auto* descriptor = MTL::CaptureDescriptor::alloc()->init();
        descriptor->setCaptureObject(_device);
        descriptor->setDestination(MTL::CaptureDestinationGPUTraceDocument);
        descriptor->setOutputURL(NS::URL::fileURLWithPath(NS::String::string(path.c_str(), NS::UTF8StringEncoding)));
        NS::Error* error = nullptr;
        _gpuCaptureActive = manager->startCapture(descriptor, &error);
        descriptor->release();
        if (!_gpuCaptureActive) {
            spdlog::error("GPU capture to '{}' failed: {}", path,
                error && error->localizedDescription() ? error->localizedDescription()->utf8String() : "unknown");
            return false;
        }
        spdlog::info("GPU capture started: {}", path);
        return true;
    }

    void MetalGraphicsDevice::endGpuCapture()
    {
        if (!_gpuCaptureActive) {
            return;
        }
        // The frame's work still in the open command buffer belongs in the capture.
        flushCommands();
        MTL::CaptureManager::sharedCaptureManager()->stopCapture();
        _gpuCaptureActive = false;
        spdlog::info("GPU capture finished");
    }


    bool MetalGraphicsDevice::supportsCompressedFormat(const PixelFormat format) const
    {
        if (!isCompressedPixelFormat(format)) {
            return true;
        }
        if (!_device) {
            return false;
        }
        switch (format) {
            case PixelFormat::PIXELFORMAT_ASTC_4x4:
            case PixelFormat::PIXELFORMAT_ASTC_5x5:
            case PixelFormat::PIXELFORMAT_ASTC_6x6:
            case PixelFormat::PIXELFORMAT_ASTC_8x8:
            case PixelFormat::PIXELFORMAT_ASTC_10x10:
            case PixelFormat::PIXELFORMAT_ASTC_12x12:
                // ASTC LDR is part of the Apple GPU family from Apple2 on.
                return _device->supportsFamily(MTL::GPUFamilyApple2);
            case PixelFormat::PIXELFORMAT_DXT1:
            case PixelFormat::PIXELFORMAT_DXT3:
            case PixelFormat::PIXELFORMAT_DXT5:
            case PixelFormat::PIXELFORMAT_BC4:
            case PixelFormat::PIXELFORMAT_BC5:
            case PixelFormat::PIXELFORMAT_BC6H:
            case PixelFormat::PIXELFORMAT_BC7:
                return _device->supportsBCTextureCompression();
            default:
                return false;
        }
    }

    void MetalGraphicsDevice::addBackendResourceCounts(LiveResourceCounts& counts) const
    {
        counts.renderPipelines = _renderPipeline ? _renderPipeline->count() : 0;
        counts.computePipelines = _computePipeline ? _computePipeline->count() : 0;
    }
}
