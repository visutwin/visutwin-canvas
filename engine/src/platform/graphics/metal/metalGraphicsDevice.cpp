// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025.
//
#include "metalGraphicsDevice.h"

#include <algorithm>
#include <cstring>
#include "metalCoCPass.h"
#include "metalComposePass.h"
#include "metalDepthAwareBlurPass.h"
#include "metalDofBlurPass.h"
#include "metalVsmBlurPass.h"
#include "metalEnvConvolvePass.h"
#include "metalEnvReprojectPass.h"
#include "metalEquirectToCubePass.h"
#include "metalInstanceCullPass.h"
#include "metalSsaoPass.h"
#include "metalTaaPass.h"
#include "metalTexture.h"
#include "metalComputePipeline.h"
#include "metalIndexBuffer.h"
#include "metalShader.h"
#include "metalRenderPipeline.h"
#include "metalRenderTarget.h"
#include "metalUtils.h"
#include "metalVertexBuffer.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    // Import metal utility functions into this translation unit for brevity.
    using metal::toMetalPrimitiveType;
    using metal::toMetalCullMode;
    using metal::createDepthTexture;
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
            float normalSign;
            float _pad[3];
        };
    }

    constexpr int INDIRECT_ENTRY_BYTE_SIZE = 5 * 4; // 5 x 32bit

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

        _commandQueue = _device->newCommandQueue();
        assert(_commandQueue && "Failed to create Metal command queue");

        auto* samplerDesc = MTL::SamplerDescriptor::alloc()->init();
        samplerDesc->setMinFilter(MTL::SamplerMinMagFilterLinear);
        samplerDesc->setMagFilter(MTL::SamplerMinMagFilterLinear);
        // Enable trilinear mipmap filtering so textures with multiple mip levels get proper
        // minification; without it Metal only ever samples mip 0 → aliasing + radial streaks
        // at glancing view angles (e.g. ground planes viewed from low camera height).
        samplerDesc->setMipFilter(MTL::SamplerMipFilterLinear);
        // 16x anisotropic filtering preserves detail on textures viewed at oblique angles —
        // essential for ground/floor textures that otherwise smear into radial lines from the
        // viewer's nadir point. Metal supports 1..16; higher costs more bandwidth but the
        // quality improvement is large for ground/terrain scenes.
        samplerDesc->setMaxAnisotropy(16);
        samplerDesc->setSAddressMode(MTL::SamplerAddressModeRepeat);
        samplerDesc->setTAddressMode(MTL::SamplerAddressModeRepeat);
        _defaultSampler = _device->newSamplerState(samplerDesc);
        samplerDesc->release();

        auto* depthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        // Default depth function is FUNC_LESSEQUAL.
        // This is critical for the skybox, which renders at depth ≈ 1.0 and needs
        // LESSEQUAL to pass the depth test against the cleared depth buffer (also 1.0).
        depthDesc->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        depthDesc->setDepthWriteEnabled(true);
        _defaultDepthStencilState = _device->newDepthStencilState(depthDesc);

        // depth-test-only state (no depth writes).
        // Used by transparent materials and the skybox that need depth testing
        // but should not write to the depth buffer.
        depthDesc->setDepthWriteEnabled(false);
        _noWriteDepthStencilState = _device->newDepthStencilState(depthDesc);
        depthDesc->release();

        _renderPipeline = std::make_unique<MetalRenderPipeline>(this);
        _computePipeline = std::make_unique<MetalComputePipeline>(this);

        // Triple-buffered ring buffers for per-draw uniform data.
        // ModelData is 136B → aligns to 256B slots.
        // LightingUniforms is the largest struct at ~544B → aligns to 768B slots.
        // MaterialUniforms (~48B) also uses the uniform ring (fits within 768B slot).
        //
        // _transformRing: 1 allocation per draw call (ModelData at slot 2).
        // _uniformRing:   2 allocations per draw call (MaterialUniforms at slot 3 +
        //                 LightingUniforms at slot 4), so needs 2× the draw capacity.
        static constexpr size_t kMaxDrawsPerFrame = 4096;
        _transformRing = std::make_unique<MetalUniformRingBuffer>(
            _device, kMaxDrawsPerFrame, sizeof(ModelData), "TransformRing");
        _uniformRing = std::make_unique<MetalUniformRingBuffer>(
            _device, kMaxDrawsPerFrame * 2, sizeof(MetalUniformBinder::LightingUniforms), "UniformRing");

        // Variable-size bump-allocator ring buffer for dynamic batch matrix palettes.
        // 256KB per frame region supports up to 4096 total instances across all batches.
        _paletteRing = std::make_unique<MetalPaletteRingBuffer>(_device, "PaletteRing");

        RenderTargetOptions backBufferOptions;
        backBufferOptions.graphicsDevice = this;
        backBufferOptions.name = "MetalBackBuffer";
        backBufferOptions.samples = 1;
        setBackBuffer(std::make_shared<MetalBackBufferRenderTarget>(backBufferOptions));
    }

    MetalGraphicsDevice::~MetalGraphicsDevice()
    {
        if (_renderPassEncoder) {
            _renderPassEncoder->endEncoding();
            _renderPassEncoder = nullptr;
        }

        // _pipelineState is a non-owning pointer; the render pipeline cache
        // owns pipeline states and releases them in its destructor.
        _pipelineState = nullptr;

        if (_defaultSampler) {
            _defaultSampler->release();
            _defaultSampler = nullptr;
        }

        if (_defaultDepthStencilState) {
            _defaultDepthStencilState->release();
            _defaultDepthStencilState = nullptr;
        }
        if (_noWriteDepthStencilState) {
            _noWriteDepthStencilState->release();
            _noWriteDepthStencilState = nullptr;
        }
        // All envAtlas passes must be destroyed before _composePass — they
        // reference it.
        _equirectToCubePass.reset();
        _envConvolvePass.reset();
        _envReprojectPass.reset();
        _taaPass.reset();
        _composePass.reset();

        if (_backBufferDepthTexture) {
            _backBufferDepthTexture->release();
            _backBufferDepthTexture = nullptr;
        }

        if (_clusterLightBuffer) {
            _clusterLightBuffer->release();
            _clusterLightBuffer = nullptr;
        }
        if (_clusterCellBuffer) {
            _clusterCellBuffer->release();
            _clusterCellBuffer = nullptr;
        }

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
        return {w, h};
    }

    void MetalGraphicsDevice::onFrameStart()
    {
        if (!_metalLayer) {
            return;
        }

        // Push a fresh autorelease pool for this frame's metal-cpp
        // autoreleased objects (command buffers, render pass descriptors,
        // drawable queries, etc.). Drained in onFrameEnd — same-frame
        // scope only. Earlier revisions kept the pool alive across frames
        // and drained at the next frameStart, but that is incompatible
        // with delegate-driven hosts (e.g. AppKit MTKView) whose outer
        // autorelease pool pops the nested canvas pool when the delegate
        // callback returns.
        _framePool = NS::AutoreleasePool::alloc()->init();

        // Advance ring buffers to next frame region. This blocks if the GPU
        // hasn't finished with the region we're about to write to.
        _transformRing->beginFrame();
        _uniformRing->beginFrame();
        _paletteRing->beginFrame();
        _pendingPaletteOffset = SIZE_MAX;

        // Release the previous frame's cached drawable so the first back-buffer
        // render pass of this frame acquires a fresh one.
        _frameDrawable = nullptr;

        const auto [w, h] = size();
        const CGSize drawableSize = _metalLayer->drawableSize();
        if (static_cast<int>(drawableSize.width) != w || static_cast<int>(drawableSize.height) != h) {
            _metalLayer->setDrawableSize(CGSize{static_cast<CGFloat>(w), static_cast<CGFloat>(h)});
        }
    }

    void MetalGraphicsDevice::onFrameEnd()
    {
        // Always commit an end-of-frame command buffer with ring buffer completion
        // handlers so the dispatch semaphores are signaled.  If no back-buffer pass
        // occurred this frame (e.g. post-processing toggled off with stale render
        // actions), _frameDrawable may be null.  We must still signal the semaphores
        // — otherwise beginFrame() blocks forever after kMaxInflightFrames.
        if (_commandQueue) {
            auto* endBuffer = _commandQueue->commandBuffer();
            if (endBuffer) {
                // Register ring buffer completion handlers on this command buffer.
                // This is the LAST command buffer committed per frame, so the
                // semaphore signals correctly track whole-frame GPU completion.
                _transformRing->endFrame(endBuffer);
                _uniformRing->endFrame(endBuffer);
                _paletteRing->endFrame(endBuffer);

                if (_frameDrawable) {
                    endBuffer->presentDrawable(_frameDrawable);
                }
                endBuffer->commit();
            }
        }
        // The frame drawable is released at the start of the next frame.

        // Drain this frame's autorelease pool. All command buffers /
        // render pass descriptors / drawable queries created between
        // onFrameStart and onFrameEnd are committed by now, so it is
        // safe to release them as a batch.
        if (_framePool) {
            _framePool->release();
            _framePool = nullptr;
        }
    }

    int MetalGraphicsDevice::submitVertexBuffer(const std::shared_ptr<VertexBuffer>& vertexBuffer, int slot)
    {
        if (!vertexBuffer || !_renderPassEncoder) {
            return 0;
        }
        auto* vbBuffer = static_cast<MTL::Buffer*>(vertexBuffer->nativeBuffer());
        if (!vbBuffer) {
            spdlog::warn("Vertex buffer has no native Metal buffer bound");
            return 0;
        }

        _renderPassEncoder->setVertexBuffer(vbBuffer, 0, static_cast<NS::UInteger>(slot));
        return 1;
    }

    std::shared_ptr<Shader> MetalGraphicsDevice::createShader(const ShaderDefinition& definition,
        const std::string& sourceCode)
    {
        return std::make_shared<MetalShader>(this, definition, sourceCode);
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

    std::unique_ptr<InstanceCuller> MetalGraphicsDevice::createInstanceCuller()
    {
        return std::make_unique<MetalInstanceCullPass>(this);
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
        return std::make_shared<MetalRenderTarget>(options);
    }

    void MetalGraphicsDevice::executeComposePass(const ComposePassParams& params)
    {
        if (!_renderPassEncoder) return;
        if (!_composePass) _composePass = std::make_unique<MetalComposePass>(this);
        _composePass->execute(_renderPassEncoder, params, _renderPipeline.get(), renderTarget(),
            _bindGroupFormats, _defaultSampler);
    }

    void MetalGraphicsDevice::executeTAAPass(Texture* sourceTexture, Texture* historyTexture, Texture* depthTexture,
        const Matrix4& viewProjectionPrevious, const Matrix4& viewProjectionInverse,
        const std::array<float, 4>& jitters, const std::array<float, 4>& cameraParams, const bool highQuality,
        const bool historyValid)
    {
        if (!_renderPassEncoder) return;
        if (!_composePass) _composePass = std::make_unique<MetalComposePass>(this);
        if (!_taaPass) _taaPass = std::make_unique<MetalTaaPass>(this, _composePass.get());
        _taaPass->execute(_renderPassEncoder, sourceTexture, historyTexture, depthTexture,
            viewProjectionPrevious, viewProjectionInverse, jitters, cameraParams,
            highQuality, historyValid, _renderPipeline.get(), renderTarget(),
            _bindGroupFormats, _defaultSampler, _defaultDepthStencilState);
    }

    void MetalGraphicsDevice::executeSsaoPass(const SsaoPassParams& params)
    {
        if (!_renderPassEncoder) return;
        if (!_composePass) _composePass = std::make_unique<MetalComposePass>(this);
        if (!_ssaoPass) _ssaoPass = std::make_unique<MetalSsaoPass>(this, _composePass.get());
        _ssaoPass->execute(_renderPassEncoder, params, _renderPipeline.get(), renderTarget(),
            _bindGroupFormats, _defaultSampler, _defaultDepthStencilState);
    }

    void MetalGraphicsDevice::executeCoCPass(const CoCPassParams& params)
    {
        if (!_renderPassEncoder) return;
        if (!_composePass) _composePass = std::make_unique<MetalComposePass>(this);
        if (!_cocPass) _cocPass = std::make_unique<MetalCoCPass>(this, _composePass.get());
        _cocPass->execute(_renderPassEncoder, params, _renderPipeline.get(), renderTarget(),
            _bindGroupFormats, _defaultSampler, _defaultDepthStencilState);
    }

    void MetalGraphicsDevice::executeDofBlurPass(const DofBlurPassParams& params)
    {
        if (!_renderPassEncoder) return;
        if (!_composePass) _composePass = std::make_unique<MetalComposePass>(this);
        if (!_dofBlurPass) _dofBlurPass = std::make_unique<MetalDofBlurPass>(this, _composePass.get());
        _dofBlurPass->execute(_renderPassEncoder, params, _renderPipeline.get(), renderTarget(),
            _bindGroupFormats, _defaultSampler, _defaultDepthStencilState);
    }

    void MetalGraphicsDevice::executeDepthAwareBlurPass(const DepthAwareBlurPassParams& params, const bool horizontal)
    {
        if (!_renderPassEncoder) return;
        if (!_composePass) _composePass = std::make_unique<MetalComposePass>(this);
        if (horizontal) {
            if (!_blurPassH) _blurPassH = std::make_unique<MetalDepthAwareBlurPass>(this, _composePass.get(), true);
            _blurPassH->execute(_renderPassEncoder, params, _renderPipeline.get(), renderTarget(),
                _bindGroupFormats, _defaultSampler, _defaultDepthStencilState);
        } else {
            if (!_blurPassV) _blurPassV = std::make_unique<MetalDepthAwareBlurPass>(this, _composePass.get(), false);
            _blurPassV->execute(_renderPassEncoder, params, _renderPipeline.get(), renderTarget(),
                _bindGroupFormats, _defaultSampler, _defaultDepthStencilState);
        }
    }

    void MetalGraphicsDevice::executeVsmBlurPass(const VsmBlurPassParams& params, const bool horizontal)
    {
        if (!_renderPassEncoder) return;
        if (!_composePass) _composePass = std::make_unique<MetalComposePass>(this);
        if (horizontal) {
            if (!_vsmBlurPassH) _vsmBlurPassH = std::make_unique<MetalVsmBlurPass>(this, _composePass.get(), true);
            _vsmBlurPassH->execute(_renderPassEncoder, params, _renderPipeline.get(), renderTarget(),
                _bindGroupFormats, _defaultDepthStencilState);
        } else {
            if (!_vsmBlurPassV) _vsmBlurPassV = std::make_unique<MetalVsmBlurPass>(this, _composePass.get(), false);
            _vsmBlurPassV->execute(_renderPassEncoder, params, _renderPipeline.get(), renderTarget(),
                _bindGroupFormats, _defaultDepthStencilState);
        }
    }

    void MetalGraphicsDevice::computeDispatch(const std::vector<Compute*>& computes, const std::string& label)
    {
        if (computes.empty() || !_commandQueue || !_device) {
            return;
        }

        if (_insideRenderPass || _renderPassEncoder || _computePassEncoder) {
            spdlog::warn("Skipping compute dispatch while a render/compute encoder is active");
            return;
        }

        auto* commandBuffer = _commandQueue->commandBuffer();
        if (!commandBuffer) {
            spdlog::warn("Failed to allocate command buffer for compute dispatch");
            return;
        }

        auto* encoder = commandBuffer->computeCommandEncoder();
        if (!encoder) {
            spdlog::warn("Failed to allocate compute encoder");
            return;
        }

        if (!label.empty()) {
            encoder->pushDebugGroup(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
        }

        for (const auto* compute : computes) {
            if (!compute || !compute->shader()) {
                continue;
            }

            auto* pipelineState = _computePipeline->get(compute->shader());
            if (!pipelineState) {
                continue;
            }

            encoder->setComputePipelineState(pipelineState);

            // DEVIATION: C++ port does not yet have compute bind group
            // declarations/reflection, so we bind textures in deterministic name order.
            std::vector<std::pair<std::string, Texture*>> textureBindings;
            textureBindings.reserve(compute->textureParameters().size());
            for (const auto& [name, texture] : compute->textureParameters()) {
                textureBindings.emplace_back(name, texture);
            }
            std::sort(textureBindings.begin(), textureBindings.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            uint32_t slot = 0u;
            for (const auto& [_, texture] : textureBindings) {
                auto* hwTexture = texture ? dynamic_cast<gpu::MetalTexture*>(texture->impl()) : nullptr;
                encoder->setTexture(hwTexture ? hwTexture->raw() : nullptr, static_cast<NS::UInteger>(slot));
                ++slot;
            }

            const MTL::Size threadgroups(
                static_cast<NS::UInteger>(compute->dispatchX()),
                static_cast<NS::UInteger>(compute->dispatchY()),
                static_cast<NS::UInteger>(compute->dispatchZ())
            );

            // DEVIATION: workgroup size is currently fixed to 8x8x1 for parity with
            // Edge-detect compute shader.
            const MTL::Size threadsPerThreadgroup(8, 8, 1);
            encoder->dispatchThreadgroups(threadgroups, threadsPerThreadgroup);
        }

        if (!label.empty()) {
            encoder->popDebugGroup();
        }

        encoder->endEncoding();
        commandBuffer->commit();
    }

    void MetalGraphicsDevice::generateEnvReproject(const EnvReprojectPassParams& params)
    {
        if (!_commandQueue || !_device) {
            spdlog::warn("generateEnvReproject: device/queue unavailable");
            return;
        }
        if (!params.target) {
            spdlog::warn("generateEnvReproject: target texture is null");
            return;
        }
        if (params.ops.empty()) {
            spdlog::warn("generateEnvReproject: no ops to bake");
            return;
        }
        if (!params.sourceEquirect && !params.sourceCubemap) {
            spdlog::warn("generateEnvReproject: no source texture provided");
            return;
        }
        if (_insideRenderPass || _renderPassEncoder || _computePassEncoder) {
            spdlog::warn("generateEnvReproject: skipped while another encoder is active");
            return;
        }

        params.target->upload();
        if (params.sourceEquirect) params.sourceEquirect->upload();
        if (params.sourceCubemap)  params.sourceCubemap->upload();

        auto* targetHw = dynamic_cast<gpu::MetalTexture*>(params.target->impl());
        if (!targetHw || !targetHw->raw()) {
            spdlog::error("generateEnvReproject: target has no Metal texture");
            return;
        }

        RenderTargetOptions rtOptions;
        rtOptions.graphicsDevice = this;
        rtOptions.colorBuffer = params.target;
        rtOptions.depth = false;
        rtOptions.samples = 1;
        rtOptions.name = "envReprojectTarget";
        rtOptions.flipY = false;
        auto renderTarget = createRenderTarget(rtOptions);
        auto metalTarget = std::dynamic_pointer_cast<MetalRenderTarget>(renderTarget);
        if (!metalTarget) {
            spdlog::error("generateEnvReproject: failed to create MetalRenderTarget");
            return;
        }
        metalTarget->ensureAttachments();

        auto* commandBuffer = _commandQueue->commandBuffer();
        if (!commandBuffer) {
            spdlog::warn("generateEnvReproject: failed to allocate command buffer");
            return;
        }

        // All ops run in this single render pass: splitting them across passes
        // with LoadActionLoad does not reliably preserve content outside the
        // scissor on Apple-Silicon tile-based GPUs.
        auto* passDesc = MTL::RenderPassDescriptor::alloc()->init();
        auto* colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(targetHw->raw());
        colorAttachment->setLoadAction(MTL::LoadActionLoad);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        auto* encoder = commandBuffer->renderCommandEncoder(passDesc);
        passDesc->release();
        if (!encoder) {
            spdlog::error("generateEnvReproject: failed to create render encoder");
            return;
        }

        if (!_envReprojectPass) {
            if (!_composePass) _composePass = std::make_unique<MetalComposePass>(this);
            _envReprojectPass = std::make_unique<MetalEnvReprojectPass>(this, _composePass.get());
        }

        _envReprojectPass->beginPass(encoder,
            params.sourceEquirect, params.sourceCubemap,
            _renderPipeline.get(), renderTarget, _bindGroupFormats);

        const bool sourceIsCubemap = params.sourceCubemap != nullptr;
        for (const auto& op : params.ops) {
            _envReprojectPass->drawRect(encoder, op, sourceIsCubemap,
                params.encodeRgbp, params.decodeSrgb);
        }

        encoder->endEncoding();
        commandBuffer->commit();
        commandBuffer->waitUntilCompleted();
    }

    void MetalGraphicsDevice::generateEnvConvolve(const EnvConvolvePassParams& params)
    {
        if (!_commandQueue || !_device) {
            spdlog::warn("generateEnvConvolve: device/queue unavailable");
            return;
        }
        if (!params.target) {
            spdlog::warn("generateEnvConvolve: target texture is null");
            return;
        }
        if (params.ops.empty()) {
            spdlog::warn("generateEnvConvolve: no ops to bake");
            return;
        }
        if (!params.sourceEquirect && !params.sourceCubemap) {
            spdlog::warn("generateEnvConvolve: no source texture provided");
            return;
        }
        if (_insideRenderPass || _renderPassEncoder || _computePassEncoder) {
            spdlog::warn("generateEnvConvolve: skipped while another encoder is active");
            return;
        }

        params.target->upload();
        if (params.sourceEquirect) params.sourceEquirect->upload();
        if (params.sourceCubemap)  params.sourceCubemap->upload();

        auto* targetHw = dynamic_cast<gpu::MetalTexture*>(params.target->impl());
        if (!targetHw || !targetHw->raw()) {
            spdlog::error("generateEnvConvolve: target has no Metal texture");
            return;
        }

        RenderTargetOptions rtOptions;
        rtOptions.graphicsDevice = this;
        rtOptions.colorBuffer = params.target;
        rtOptions.depth = false;
        rtOptions.samples = 1;
        rtOptions.name = "envConvolveTarget";
        rtOptions.flipY = false;
        auto renderTarget = createRenderTarget(rtOptions);
        auto metalTarget = std::dynamic_pointer_cast<MetalRenderTarget>(renderTarget);
        if (!metalTarget) {
            spdlog::error("generateEnvConvolve: failed to create MetalRenderTarget");
            return;
        }
        metalTarget->ensureAttachments();

        auto* commandBuffer = _commandQueue->commandBuffer();
        if (!commandBuffer) {
            spdlog::warn("generateEnvConvolve: failed to allocate command buffer");
            return;
        }

        // Single render pass for all ops — LoadActionLoad with partial scissor
        // across multiple passes does not preserve content outside the scissor
        // on Apple-Silicon tile-based GPUs.
        auto* passDesc = MTL::RenderPassDescriptor::alloc()->init();
        auto* colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(targetHw->raw());
        colorAttachment->setLoadAction(MTL::LoadActionLoad);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        auto* encoder = commandBuffer->renderCommandEncoder(passDesc);
        passDesc->release();
        if (!encoder) {
            spdlog::error("generateEnvConvolve: failed to create render encoder");
            return;
        }

        if (!_envConvolvePass) {
            if (!_composePass) _composePass = std::make_unique<MetalComposePass>(this);
            _envConvolvePass = std::make_unique<MetalEnvConvolvePass>(this, _composePass.get());
        }

        _envConvolvePass->beginPass(encoder,
            params.sourceEquirect, params.sourceCubemap,
            _renderPipeline.get(), renderTarget, _bindGroupFormats);

        const bool sourceIsCubemap = params.sourceCubemap != nullptr;
        for (const auto& op : params.ops) {
            _envConvolvePass->drawRect(encoder, op, sourceIsCubemap,
                params.encodeRgbp, params.decodeSrgb);
        }

        encoder->endEncoding();
        commandBuffer->commit();
        commandBuffer->waitUntilCompleted();
    }

    void MetalGraphicsDevice::generateEnvAtlas(const EnvAtlasBakeParams& params)
    {
        if (!_commandQueue || !_device) {
            spdlog::warn("generateEnvAtlas: device/queue unavailable");
            return;
        }
        if (!params.target) {
            spdlog::warn("generateEnvAtlas: target texture is null");
            return;
        }
        if (params.reprojectOps.empty() && params.convolveOps.empty()) {
            spdlog::warn("generateEnvAtlas: no ops to bake");
            return;
        }
        if (_insideRenderPass || _renderPassEncoder || _computePassEncoder) {
            spdlog::warn("generateEnvAtlas: skipped while another encoder is active");
            return;
        }

        params.target->upload();
        if (params.reprojectSourceEquirect) params.reprojectSourceEquirect->upload();
        if (params.reprojectSourceCubemap)  params.reprojectSourceCubemap->upload();
        if (params.convolveSourceEquirect)  params.convolveSourceEquirect->upload();
        if (params.convolveSourceCubemap)   params.convolveSourceCubemap->upload();

        auto* targetHw = dynamic_cast<gpu::MetalTexture*>(params.target->impl());
        if (!targetHw || !targetHw->raw()) {
            spdlog::error("generateEnvAtlas: target has no Metal texture");
            return;
        }

        RenderTargetOptions rtOptions;
        rtOptions.graphicsDevice = this;
        rtOptions.colorBuffer = params.target;
        rtOptions.depth = false;
        rtOptions.samples = 1;
        rtOptions.name = "envAtlasTarget";
        rtOptions.flipY = false;
        auto renderTarget = createRenderTarget(rtOptions);
        auto metalTarget = std::dynamic_pointer_cast<MetalRenderTarget>(renderTarget);
        if (!metalTarget) {
            spdlog::error("generateEnvAtlas: failed to create MetalRenderTarget");
            return;
        }
        metalTarget->ensureAttachments();

        auto* commandBuffer = _commandQueue->commandBuffer();
        if (!commandBuffer) {
            spdlog::warn("generateEnvAtlas: failed to allocate command buffer");
            return;
        }

        auto* passDesc = MTL::RenderPassDescriptor::alloc()->init();
        auto* colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(targetHw->raw());
        colorAttachment->setLoadAction(MTL::LoadActionLoad);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        auto* encoder = commandBuffer->renderCommandEncoder(passDesc);
        passDesc->release();
        if (!encoder) {
            spdlog::error("generateEnvAtlas: failed to create render encoder");
            return;
        }

        if (!_composePass) _composePass = std::make_unique<MetalComposePass>(this);

        if (!params.reprojectOps.empty()) {
            if (!_envReprojectPass) {
                _envReprojectPass = std::make_unique<MetalEnvReprojectPass>(this, _composePass.get());
            }
            _envReprojectPass->beginPass(encoder,
                params.reprojectSourceEquirect, params.reprojectSourceCubemap,
                _renderPipeline.get(), renderTarget, _bindGroupFormats);

            const bool sourceIsCubemap = params.reprojectSourceCubemap != nullptr;
            for (const auto& op : params.reprojectOps) {
                _envReprojectPass->drawRect(encoder, op, sourceIsCubemap,
                    params.encodeRgbp, params.decodeSrgb);
            }
        }

        if (!params.convolveOps.empty()) {
            if (!_envConvolvePass) {
                _envConvolvePass = std::make_unique<MetalEnvConvolvePass>(this, _composePass.get());
            }
            _envConvolvePass->beginPass(encoder,
                params.convolveSourceEquirect, params.convolveSourceCubemap,
                _renderPipeline.get(), renderTarget, _bindGroupFormats);

            const bool sourceIsCubemap = params.convolveSourceCubemap != nullptr;
            for (const auto& op : params.convolveOps) {
                _envConvolvePass->drawRect(encoder, op, sourceIsCubemap,
                    params.encodeRgbp, params.decodeSrgb);
            }
        }

        encoder->endEncoding();
        commandBuffer->commit();
        commandBuffer->waitUntilCompleted();
    }

    void MetalGraphicsDevice::generateEquirectToCubemap(const EquirectToCubeParams& params)
    {
        if (!_commandQueue || !_device) {
            spdlog::warn("generateEquirectToCubemap: device/queue unavailable");
            return;
        }
        if (!params.source || !params.target) {
            spdlog::warn("generateEquirectToCubemap: source or target is null");
            return;
        }
        if (!params.target->isCubemap()) {
            spdlog::error("generateEquirectToCubemap: target is not a cubemap");
            return;
        }
        if (_insideRenderPass || _renderPassEncoder || _computePassEncoder) {
            spdlog::warn("generateEquirectToCubemap: skipped while another encoder is active");
            return;
        }

        params.source->upload();
        params.target->upload();

        auto* targetHw = dynamic_cast<gpu::MetalTexture*>(params.target->impl());
        if (!targetHw || !targetHw->raw()) {
            spdlog::error("generateEquirectToCubemap: target has no Metal texture");
            return;
        }

        const int faceSize = static_cast<int>(params.target->width());

        if (!_composePass) _composePass = std::make_unique<MetalComposePass>(this);
        if (!_equirectToCubePass) {
            _equirectToCubePass = std::make_unique<MetalEquirectToCubePass>(this, _composePass.get());
        }

        auto* commandBuffer = _commandQueue->commandBuffer();
        if (!commandBuffer) {
            spdlog::warn("generateEquirectToCubemap: failed to allocate command buffer");
            return;
        }

        // One render pass per cube face. Each face is a distinct storage slice,
        // so the tile-memory preservation issue that forces the atlas bake into
        // a single pass does not apply here.
        for (uint32_t face = 0; face < 6u; ++face) {
            RenderTargetOptions rtOptions;
            rtOptions.graphicsDevice = this;
            rtOptions.colorBuffer = params.target;
            rtOptions.face = static_cast<int>(face);
            rtOptions.depth = false;
            rtOptions.samples = 1;
            rtOptions.name = "equirectToCubeTarget";
            rtOptions.flipY = false;
            auto renderTarget = createRenderTarget(rtOptions);
            auto metalTarget = std::dynamic_pointer_cast<MetalRenderTarget>(renderTarget);
            if (!metalTarget) {
                spdlog::error("generateEquirectToCubemap: failed to create MetalRenderTarget");
                continue;
            }
            metalTarget->ensureAttachments();

            auto* passDesc = MTL::RenderPassDescriptor::alloc()->init();
            auto* colorAttachment = passDesc->colorAttachments()->object(0);
            colorAttachment->setTexture(targetHw->raw());
            colorAttachment->setSlice(static_cast<NS::UInteger>(face));
            colorAttachment->setLoadAction(MTL::LoadActionDontCare);
            colorAttachment->setStoreAction(MTL::StoreActionStore);

            auto* encoder = commandBuffer->renderCommandEncoder(passDesc);
            passDesc->release();
            if (!encoder) {
                spdlog::error("generateEquirectToCubemap: failed to create render encoder");
                continue;
            }

            _equirectToCubePass->beginPass(encoder, params.source,
                _renderPipeline.get(), renderTarget, _bindGroupFormats);
            _equirectToCubePass->drawFace(encoder, face, faceSize, params.decodeSrgb);
            encoder->endEncoding();
        }

        if (params.target->mipmaps() && params.target->getNumLevels() > 1) {
            auto* blitEncoder = commandBuffer->blitCommandEncoder();
            if (blitEncoder) {
                blitEncoder->generateMipmaps(targetHw->raw());
                blitEncoder->endEncoding();
            }
        }

        commandBuffer->commit();
        commandBuffer->waitUntilCompleted();
    }

    void MetalGraphicsDevice::setDepthBias(const float depthBias, const float slopeScale, const float clamp)
    {
        if (!_renderPassEncoder) return;
        _renderPassEncoder->setDepthBias(depthBias, slopeScale, clamp);
    }

    void MetalGraphicsDevice::setIndirectDrawBuffer(void* nativeBuffer)
    {
        _indirectDrawBuffer = static_cast<MTL::Buffer*>(nativeBuffer);
    }

    void MetalGraphicsDevice::setDynamicBatchPalette(const void* data, const size_t size)
    {
        // Allocate from the palette ring buffer (variable-size bump allocator).
        // This replaces the previous setVertexBytes() path which was limited to 4KB.
        // The ring buffer supports arbitrary palette sizes within the 256KB/frame budget.
        _pendingPaletteOffset = _paletteRing->allocate(data, size);
    }

    void MetalGraphicsDevice::setClusterBuffers(const void* lightData, const size_t lightSize,
        const void* cellData, const size_t cellSize)
    {
        if (!lightData || lightSize == 0 || !cellData || cellSize == 0) {
            _clusterBuffersSet = false;
            return;
        }

        // Grow light buffer if needed.
        if (!_clusterLightBuffer || _clusterLightBufferCapacity < lightSize) {
            if (_clusterLightBuffer) {
                _clusterLightBuffer->release();
            }
            _clusterLightBufferCapacity = lightSize * 2; // over-allocate to reduce reallocations
            _clusterLightBuffer = _device->newBuffer(_clusterLightBufferCapacity,
                MTL::ResourceStorageModeShared);
        }

        // Grow cell buffer if needed.
        if (!_clusterCellBuffer || _clusterCellBufferCapacity < cellSize) {
            if (_clusterCellBuffer) {
                _clusterCellBuffer->release();
            }
            _clusterCellBufferCapacity = cellSize * 2;
            _clusterCellBuffer = _device->newBuffer(_clusterCellBufferCapacity,
                MTL::ResourceStorageModeShared);
        }

        // Copy data.
        std::memcpy(_clusterLightBuffer->contents(), lightData, lightSize);
        std::memcpy(_clusterCellBuffer->contents(), cellData, cellSize);

        _clusterBuffersSet = true;
    }

    void MetalGraphicsDevice::setClusterGridParams(const float* boundsMin, const float* boundsRange,
        const float* cellsCountByBoundsSize,
        const int cellsX, const int cellsY, const int cellsZ, const int maxLightsPerCell,
        const int numClusteredLights)
    {
        _uniformBinder.setClusterParams(boundsMin, boundsRange, cellsCountByBoundsSize,
            cellsX, cellsY, cellsZ, maxLightsPerCell, numClusteredLights);
    }

    void MetalGraphicsDevice::setTransformUniforms(const Matrix4& viewProjection, const Matrix4& model)
    {
        if (!_renderPassEncoder) return;
        _uniformBinder.setTransformUniforms(_renderPassEncoder, _transformRing.get(), viewProjection, model);
    }

    void MetalGraphicsDevice::setLightingUniforms(const Color& ambientColor, const std::vector<GpuLightData>& lights,
        const Vector3& cameraPosition, const bool enableNormalMaps, const float exposure,
        const FogParams& fogParams, const ShadowParams& shadowParams, const int toneMapping)
    {
        _uniformBinder.setLightingUniforms(ambientColor, lights, cameraPosition,
            enableNormalMaps, exposure, fogParams, shadowParams, toneMapping);
    }

    void MetalGraphicsDevice::setEnvironmentUniforms(Texture* envAtlas, const float skyboxIntensity,
        const float skyboxMip, const Vector3& skyDomeCenter, const bool isDome,
        Texture* skyboxCubeMap)
    {
        _uniformBinder.setEnvironmentUniforms(envAtlas, skyboxIntensity, skyboxMip,
            skyDomeCenter, isDome, skyboxCubeMap);
    }

    void MetalGraphicsDevice::setAtmosphereUniforms(const void* data, const size_t size)
    {
        _uniformBinder.setAtmosphereUniforms(data, size);
    }

    void MetalGraphicsDevice::draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer,
                int numInstances, const int indirectSlot, const bool first, const bool last)
    {
        if (!_shader || !_renderPassEncoder) {
            spdlog::warn("Draw skipped: shader or render encoder is not set");
            return;
        }

        MTL::RenderCommandEncoder* passEncoder = _renderPassEncoder;
        assert(passEncoder != nullptr);

        MTL::RenderPipelineState* pipelineState = _pipelineState;

        // Get vertex buffers — use const references to avoid shared_ptr refcount churn.
        const auto& vb0 = _vertexBuffers.size() > 0 ? _vertexBuffers[0] : _nullVertexBuffer;
        const auto& vb1 = _vertexBuffers.size() > 1 ? _vertexBuffers[1] : _nullVertexBuffer;

        // Hardware instancing: find VB with isInstancing() format (set at slot 5 by renderer).
        // Use raw pointer — the shared_ptr in _vertexBuffers keeps the object alive.
        const std::shared_ptr<VertexBuffer>* instancingVBPtr = nullptr;
        for (size_t i = 2; i < _vertexBuffers.size(); ++i) {
            if (_vertexBuffers[i] && _vertexBuffers[i]->format() &&
                _vertexBuffers[i]->format()->isInstancing()) {
                instancingVBPtr = &_vertexBuffers[i];
                break;
            }
        }

        if (first) {
            // Submit vertex buffers
            if (vb0) {
                int vbSlot = submitVertexBuffer(vb0, 0);
                if (vb1) {
                    //validateVBLocations(vb0, vb1);
                    submitVertexBuffer(vb1, vbSlot);
                }
            }

            // Submit instancing vertex buffer at slot 5 for vertex descriptor layout(5).
            if (instancingVBPtr) {
                submitVertexBuffer(*instancingVBPtr, 5);
            }

            // Validate attributes
            //validateAttributes(_shader, vb0, vb1);

            // Get or create a render pipeline (includes instancing format for perInstance step function)
            const int ibFormat = indexBuffer ? indexBuffer->format() : -1;
            const auto instFmt = instancingVBPtr ? (*instancingVBPtr)->format() : nullptr;
            pipelineState = _renderPipeline->get(primitive, vb0 != nullptr ? vb0->format() : nullptr,
                vb1 != nullptr ? vb1->format() : nullptr,
                ibFormat, _shader, _renderTarget, _bindGroupFormats, _blendState, _depthState,
                _cullMode, _stencilEnabled, _stencilFront, _stencilBack, instFmt);
            if (!pipelineState) {
                spdlog::error("Draw skipped: failed to create/render pipeline state");
                return;
            }

            // Set the pipeline state if changed.
            // NOTE: _pipelineState is a non-owning (borrowing) pointer — the render
            // pipeline cache (_renderPipeline) owns pipeline states and releases them
            // in its destructor.  Do NOT call release() here; the previous code did
            // so and caused a double-release (cache destructor also releases).
            if (_pipelineState != pipelineState) {
                _pipelineState = pipelineState;
                passEncoder->setRenderPipelineState(pipelineState);
            }
        }

        MTL::Buffer* ibBuffer = nullptr;
        MTL::IndexType indexType = MTL::IndexTypeUInt16;
        if (indexBuffer) {
            ibBuffer = static_cast<MTL::Buffer*>(indexBuffer->nativeBuffer());
            if (!ibBuffer) {
                spdlog::warn("Draw skipped: index buffer has no native Metal buffer");
                return;
            }
            switch (indexBuffer->format()) {
            case INDEXFORMAT_UINT16:
                indexType = MTL::IndexTypeUInt16;
                break;
            case INDEXFORMAT_UINT32:
                indexType = MTL::IndexTypeUInt32;
                break;
            case INDEXFORMAT_UINT8:
                // Metal does not support uint8 indices directly.
                spdlog::warn("Draw skipped: uint8 index buffers are not supported on Metal");
                return;
            default:
                indexType = MTL::IndexTypeUInt16;
                break;
            }
        }

        // ── Common setup (always runs) ───────────────────────────────

        if (_cullMode == CullMode::CULLFACE_FRONTANDBACK) {
            return;
        }
        // glTF (and upstream/WebGL) use counter-clockwise front faces by default.
        passEncoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
        passEncoder->setCullMode(toMetalCullMode(_cullMode));

        MaterialUniforms materialUniforms;
        const auto* boundMaterial = material();

        // Uniform data: use customUniformData() if available (e.g., globe tiles
        // with extended uniforms), otherwise fall back to standard updateUniforms().
        const void* uniformData = &materialUniforms;
        size_t uniformSize = sizeof(MaterialUniforms);

        if (boundMaterial) {
            size_t customSize = 0;
            const void* customData = boundMaterial->customUniformData(customSize);
            if (customData && customSize > 0) {
                uniformData = customData;
                uniformSize = customSize;
            } else {
                boundMaterial->updateUniforms(materialUniforms);
            }

            // Skip texture rebinding when same material is still bound.
            if (_uniformBinder.isMaterialChanged(boundMaterial)) {
                std::vector<TextureSlot> textureSlots;
                boundMaterial->getTextureSlots(textureSlots);
                _textureBinder.bindMaterialTextures(passEncoder, textureSlots);
            }
        } else {
            _textureBinder.clearAllMaterialSlots(passEncoder);
        }

        if (quadRenderActive()) {
            _textureBinder.bindQuadTextures(passEncoder, quadTextureBindings());
        } else {
            _textureBinder.bindSceneTextures(passEncoder,
                _uniformBinder.envAtlasTexture(), _uniformBinder.shadowTexture(),
                sceneDepthMap(), _uniformBinder.skyboxCubeMapTexture(),
                reflectionMap(), reflectionDepthMap(), ssaoForwardTexture());
            _textureBinder.bindLocalShadowTextures(passEncoder,
                _uniformBinder.localShadowTexture0(), _uniformBinder.localShadowTexture1());
            _textureBinder.bindOmniShadowTextures(passEncoder,
                _uniformBinder.omniShadowCube0(), _uniformBinder.omniShadowCube1());
        }

        // Pack screen inverse resolution for planar reflection screen-space UV.
        _uniformBinder.setScreenResolution(vw(), vh());

        // Pack blurred planar reflection parameters.
        {
            const auto& rbp = reflectionBlurParams();
            _uniformBinder.setReflectionBlurParams(
                rbp.intensity, rbp.blurAmount, rbp.fadeStrength, rbp.angleFade,
                rbp.fadeColor.r, rbp.fadeColor.g, rbp.fadeColor.b);
            _uniformBinder.setReflectionDepthParams(rbp.planeDistance, rbp.heightRange);
        }

        _uniformBinder.submitPerDrawUniforms(passEncoder, _uniformRing.get(),
            boundMaterial, uniformData, uniformSize, hdrPass());

        // Bind atmosphere uniforms at fragment slot 9 for skybox draws when atmosphere is enabled.
        if (atmosphereEnabled() && boundMaterial && boundMaterial->isSkybox()) {
            const auto& atmoUniforms = _uniformBinder.atmosphereUniforms();
            passEncoder->setFragmentBytes(&atmoUniforms, sizeof(atmoUniforms), 9);
        }

        _textureBinder.bindSamplerCached(passEncoder, _defaultSampler);

        // After the first draw in a pass has established all texture/sampler state,
        // subsequent draws can rely on the cache for deduplication.
        _textureBinder.markClean();

        // per-draw-call depth stencil state switching.
        // Transparent materials (e.g. shadow catcher) may need depthWrite disabled.
        const bool needsNoWrite = _depthState && !_depthState->depthWrite();
        if (needsNoWrite && _noWriteDepthStencilState) {
            passEncoder->setDepthStencilState(_noWriteDepthStencilState);
        }

        // ── Dynamic batch palette binding (slot 6) ─────────────────────
        // Update the palette ring buffer offset for this draw call.
        // The buffer itself is bound once per render pass in startRenderPass();
        // here we only update the offset (cheap, no validation).
        // Uses Metal buffer (slot 6) for bone data.
        if (_pendingPaletteOffset != SIZE_MAX) {
            passEncoder->setVertexBufferOffset(_pendingPaletteOffset, 6);
            _pendingPaletteOffset = SIZE_MAX;
        }

        // ── Draw dispatch (branch: indirect vs direct) ────────────────

        const auto primitiveType = toMetalPrimitiveType(primitive.type);

        if (indirectSlot >= 0 && _indirectDrawBuffer) {
            // GPU-driven indirect draw: instance count comes from the GPU.
            const auto indirectOffset = static_cast<NS::UInteger>(indirectSlot * INDIRECT_ENTRY_BYTE_SIZE);
            if (indexBuffer) {
                passEncoder->drawIndexedPrimitives(
                    primitiveType, indexType, ibBuffer, 0,
                    _indirectDrawBuffer, indirectOffset);
            } else {
                passEncoder->drawPrimitives(
                    primitiveType, _indirectDrawBuffer, indirectOffset);
            }
            _indirectDrawBuffer = nullptr;  // Consumed
        } else {
            // Direct draw (standard path)
            if (indexBuffer) {
                const auto indexElementSize = (indexType == MTL::IndexTypeUInt32) ? 4 : 2;
                const auto indexBufferOffset = static_cast<NS::UInteger>(primitive.base * indexElementSize);
                passEncoder->drawIndexedPrimitives(
                    primitiveType,
                    static_cast<NS::UInteger>(primitive.count),
                    indexType,
                    ibBuffer,
                    indexBufferOffset,
                    static_cast<NS::UInteger>(numInstances),
                    static_cast<NS::Integer>(primitive.baseVertex),
                    0
                );
            } else {
                passEncoder->drawPrimitives(
                    primitiveType,
                    static_cast<NS::UInteger>(primitive.base),
                    static_cast<NS::UInteger>(primitive.count),
                    static_cast<NS::UInteger>(numInstances)
                );
            }
        }

        // Restore default depth stencil state after draw
        if (needsNoWrite && _defaultDepthStencilState) {
            passEncoder->setDepthStencilState(_defaultDepthStencilState);
        }

        recordDrawCall();

        if (last) {
            // Clear vertex buffer array
            clearVertexBuffer();
            _pipelineState = nullptr;
        }
    }

    void MetalGraphicsDevice::startRenderPass(RenderPass* renderPass)
    {
        if (!_commandQueue || !_metalLayer || _renderPassEncoder) {
            spdlog::warn("Cannot start render pass: queue/layer invalid or encoder already active");
            return;
        }

        const std::shared_ptr<RenderTarget> activeTarget =
            (renderPass && renderPass->renderTarget()) ? renderPass->renderTarget() : backBuffer();
        setRenderTarget(activeTarget);
        const auto offscreenTarget = std::dynamic_pointer_cast<MetalRenderTarget>(activeTarget);
        const bool isBackBufferPass = activeTarget == backBuffer();
        if (!isBackBufferPass && !offscreenTarget) {
            spdlog::error("Non-backbuffer render target is not a MetalRenderTarget");
            return;
        }
        if (offscreenTarget) {
            offscreenTarget->ensureAttachments();
        }

        _currentDrawable = nullptr;
        if (isBackBufferPass) {
            // Reuse the frame's cached drawable so that multiple back-buffer render
            // passes within one frame write to the same drawable texture.  Metal's
            // nextDrawable() returns a *different* drawable each call (unlike WebGL's
            // persistent back buffer), so acquiring one per pass would cause only the
            // last pass's content to be visible.
            if (_frameDrawable) {
                _currentDrawable = _frameDrawable;
                spdlog::trace("Reusing cached CAMetalDrawable for back-buffer pass");
            } else {
                _currentDrawable = _metalLayer->nextDrawable();
                if (!_currentDrawable) {
                    spdlog::warn("Failed to acquire CAMetalDrawable");
                    return;
                }
                _frameDrawable = _currentDrawable;
                spdlog::trace("Acquired new CAMetalDrawable for frame");
            }
        }

        _commandBuffer = _commandQueue->commandBuffer();
        if (!_commandBuffer) {
            spdlog::error("Failed to create Metal command buffer");
            _currentDrawable = nullptr;
            return;
        }

        auto* passDesc = MTL::RenderPassDescriptor::alloc()->init();

        const auto& colorOpsArray = renderPass ? renderPass->colorArrayOps() : std::vector<std::shared_ptr<ColorAttachmentOps>>{};
        const auto depthOps = renderPass ? renderPass->depthStencilOps() : nullptr;
        const bool canResolve = activeTarget && activeTarget->samples() > 1 && activeTarget->autoResolve();

        auto resolveColorStoreAction = [](bool store, bool resolve) {
            if (resolve) {
                return store ? MTL::StoreActionStoreAndMultisampleResolve : MTL::StoreActionMultisampleResolve;
            }
            return store ? MTL::StoreActionStore : MTL::StoreActionDontCare;
        };

        if (isBackBufferPass) {
            auto* colorAttachment = passDesc->colorAttachments()->object(0);
            colorAttachment->setTexture(_currentDrawable->texture());

            const auto colorOps = renderPass ? renderPass->colorOps() : nullptr;
            if (colorOps && colorOps->clear) {
                const auto c = colorOps->clearValue;
                colorAttachment->setLoadAction(MTL::LoadActionClear);
                colorAttachment->setClearColor(MTL::ClearColor::Make(c.r, c.g, c.b, c.a));
            } else {
                colorAttachment->setLoadAction(MTL::LoadActionLoad);
            }
            colorAttachment->setStoreAction(colorOps && colorOps->store ? MTL::StoreActionStore : MTL::StoreActionDontCare);

            const auto drawableWidth = static_cast<int>(_currentDrawable->texture()->width());
            const auto drawableHeight = static_cast<int>(_currentDrawable->texture()->height());
            if (!_backBufferDepthTexture ||
                _backBufferDepthWidth != drawableWidth ||
                _backBufferDepthHeight != drawableHeight) {
                if (_backBufferDepthTexture) {
                    _backBufferDepthTexture->release();
                    _backBufferDepthTexture = nullptr;
                }
                _backBufferDepthTexture = createDepthTexture(_device, drawableWidth, drawableHeight);
                _backBufferDepthWidth = drawableWidth;
                _backBufferDepthHeight = drawableHeight;
            }

            if (_backBufferDepthTexture) {
                auto* depthAttachment = passDesc->depthAttachment();
                depthAttachment->setTexture(_backBufferDepthTexture);
                if (depthOps && depthOps->clearDepth) {
                    depthAttachment->setLoadAction(MTL::LoadActionClear);
                    depthAttachment->setClearDepth(depthOps->clearDepthValue);
                } else {
                    depthAttachment->setLoadAction(MTL::LoadActionLoad);
                }
                depthAttachment->setStoreAction(depthOps && depthOps->storeDepth
                    ? MTL::StoreActionStore
                    : MTL::StoreActionDontCare);
            }
        } else {
            const auto& colorAttachments = offscreenTarget->colorAttachments();
            for (size_t i = 0; i < colorAttachments.size(); ++i) {
                const auto& attachment = colorAttachments[i];
                if (!attachment || !attachment->texture) {
                    continue;
                }

                auto* colorAttachment = passDesc->colorAttachments()->object(static_cast<NS::UInteger>(i));
                const bool multisampled = attachment->multisampledBuffer != nullptr;
                colorAttachment->setTexture(multisampled ? attachment->multisampledBuffer : attachment->texture);

                const auto ops = i < colorOpsArray.size() ? colorOpsArray[i] : nullptr;
                if (ops && ops->clear) {
                    const auto c = ops->clearValue;
                    colorAttachment->setLoadAction(MTL::LoadActionClear);
                    colorAttachment->setClearColor(MTL::ClearColor::Make(c.r, c.g, c.b, c.a));
                } else {
                    colorAttachment->setLoadAction(MTL::LoadActionLoad);
                }

                const bool resolve = multisampled && canResolve && ops && ops->resolve;
                if (resolve) {
                    colorAttachment->setResolveTexture(attachment->texture);
                }
                colorAttachment->setStoreAction(resolveColorStoreAction(ops ? ops->store : true, resolve));
            }

            const auto& depthAttachmentData = offscreenTarget->depthAttachment();
            if (depthAttachmentData && depthAttachmentData->depthTexture) {
                auto* depthAttachment = passDesc->depthAttachment();
                const bool depthMsaa = depthAttachmentData->multisampledDepthBuffer != nullptr;
                MTL::Texture* depthTex = depthMsaa ? depthAttachmentData->multisampledDepthBuffer : depthAttachmentData->depthTexture;
                depthAttachment->setTexture(depthTex);

                // Cubemap face rendering: when the depth texture is a cubemap, target a
                // specific face via the slice parameter.  This enables rendering to individual
                // faces of a point-light shadow cubemap.
                if (depthTex->textureType() == MTL::TextureTypeCube && activeTarget->face() >= 0) {
                    depthAttachment->setSlice(static_cast<NS::UInteger>(activeTarget->face()));
                }

                if (depthOps && depthOps->clearDepth) {
                    depthAttachment->setLoadAction(MTL::LoadActionClear);
                    depthAttachment->setClearDepth(depthOps->clearDepthValue);
                } else {
                    depthAttachment->setLoadAction(MTL::LoadActionLoad);
                }

                const bool resolveDepth = depthMsaa && canResolve && depthOps && depthOps->resolveDepth;
                if (resolveDepth) {
                    depthAttachment->setResolveTexture(depthAttachmentData->depthTexture);
                }
                depthAttachment->setStoreAction(resolveColorStoreAction(depthOps ? depthOps->storeDepth : true, resolveDepth));

                if (depthAttachmentData->hasStencil) {
                    auto* stencilAttachment = passDesc->stencilAttachment();
                    stencilAttachment->setTexture(depthMsaa ? depthAttachmentData->multisampledDepthBuffer : depthAttachmentData->depthTexture);
                    if (depthOps && depthOps->clearStencil) {
                        stencilAttachment->setLoadAction(MTL::LoadActionClear);
                        stencilAttachment->setClearStencil(depthOps->clearStencilValue);
                    } else {
                        stencilAttachment->setLoadAction(MTL::LoadActionLoad);
                    }
                    stencilAttachment->setStoreAction(depthOps && depthOps->storeStencil ? MTL::StoreActionStore : MTL::StoreActionDontCare);
                }
            }
        }

        _renderPassEncoder = _commandBuffer->renderCommandEncoder(passDesc);
        if (!_renderPassEncoder) {
            spdlog::error("Failed to create Metal render command encoder");
            _commandBuffer = nullptr;
            _currentDrawable = nullptr;
            _insideRenderPass = false;
        } else {
            if (_defaultDepthStencilState) {
                _renderPassEncoder->setDepthStencilState(_defaultDepthStencilState);
            }
            const int targetWidth = activeTarget ? activeTarget->width() : size().first;
            const int targetHeight = activeTarget ? activeTarget->height() : size().second;
            if (targetWidth > 0 && targetHeight > 0) {
                setViewport(0.0f, 0.0f, static_cast<float>(targetWidth), static_cast<float>(targetHeight));
                setScissor(0, 0, targetWidth, targetHeight);
            }

            // Bind ring buffers once per render pass. Per-draw calls will only
            // update the offset (setVertexBufferOffset), which is much cheaper
            // than rebinding the buffer + validating it each time.
            _renderPassEncoder->setVertexBuffer(_transformRing->buffer(), 0, 2);
            _renderPassEncoder->setFragmentBuffer(_uniformRing->buffer(), 0, 3);
            _renderPassEncoder->setVertexBuffer(_uniformRing->buffer(), 0, 3);
            _renderPassEncoder->setFragmentBuffer(_uniformRing->buffer(), 0, 4);
            _renderPassEncoder->setVertexBuffer(_paletteRing->buffer(), 0, 6);

            // Bind clustered lighting buffers at fragment slots 7 (lights) and 8 (cells).
            if (_clusterBuffersSet && _clusterLightBuffer && _clusterCellBuffer) {
                _renderPassEncoder->setFragmentBuffer(_clusterLightBuffer, 0, 7);
                _renderPassEncoder->setFragmentBuffer(_clusterCellBuffer, 0, 8);
            }

            // Reset per-pass deduplication state for uniforms and textures.
            _uniformBinder.resetPassState();
            _textureBinder.resetPassState();

            _insideRenderPass = true;
        }
        passDesc->release();
    }

    void MetalGraphicsDevice::endRenderPass(RenderPass* renderPass)
    {
        if (_renderPassEncoder) {
            _renderPassEncoder->endEncoding();
            _renderPassEncoder = nullptr;
        }
        _insideRenderPass = false;

        const auto activeTarget = renderTarget();
        const auto offscreenTarget = std::dynamic_pointer_cast<MetalRenderTarget>(activeTarget);
        if (_commandBuffer && offscreenTarget && renderPass) {
            bool needsMipmaps = false;
            const auto& colorOpsArray = renderPass->colorArrayOps();
            for (size_t i = 0; i < colorOpsArray.size(); ++i) {
                const auto ops = colorOpsArray[i];
                const auto* colorBuffer = activeTarget && i < static_cast<size_t>(activeTarget->colorBufferCount())
                    ? activeTarget->getColorBuffer(i) : nullptr;
                if (ops && ops->genMipmaps && colorBuffer && colorBuffer->mipmaps()) {
                    needsMipmaps = true;
                    break;
                }
            }

            if (needsMipmaps) {
                auto* blitEncoder = _commandBuffer->blitCommandEncoder();
                if (blitEncoder) {
                    for (size_t i = 0; i < colorOpsArray.size(); ++i) {
                        const auto ops = colorOpsArray[i];
                        const auto* colorBuffer = activeTarget && i < static_cast<size_t>(activeTarget->colorBufferCount())
                            ? activeTarget->getColorBuffer(i) : nullptr;
                        if (!(ops && ops->genMipmaps && colorBuffer && colorBuffer->mipmaps())) {
                            continue;
                        }
                        auto* hwTexture = dynamic_cast<gpu::MetalTexture*>(colorBuffer->impl());
                        if (hwTexture && hwTexture->raw()) {
                            blitEncoder->generateMipmaps(hwTexture->raw());
                        }
                    }
                    blitEncoder->endEncoding();
                }
            }
        }

        if (_commandBuffer) {
            // DEVIATION: in WebGL/WebGPU, the back buffer persists across
            // render passes within a frame and is presented once at frame end (swap).
            // In Metal, each back-buffer render pass gets a separate command buffer.
            // We defer presentDrawable() to onFrameEnd() so that only the final
            // back-buffer command buffer presents the drawable.  Calling it here would
            // cause the compose pass to present before the after-pass finishes.
            spdlog::trace("Committing Metal command buffer (present deferred to frame end)");
            _commandBuffer->commit();
        } else {
            spdlog::warn("Render pass ended without a valid command buffer");
        }

        _commandBuffer = nullptr;
        _currentDrawable = nullptr;
    }

    void MetalGraphicsDevice::setResolution(int width, int height)
    {
        if (_metalLayer) {
            _metalLayer->setDrawableSize(CGSize{static_cast<CGFloat>(width), static_cast<CGFloat>(height)});
        }
    }

    void MetalGraphicsDevice::setViewport(float x, float y, float w, float h)
    {
        GraphicsDevice::setViewport(x, y, w, h);
        if (_renderPassEncoder && w > 0.0f && h > 0.0f) {
            MTL::Viewport viewport;
            viewport.originX = static_cast<double>(x);
            viewport.originY = static_cast<double>(y);
            viewport.width = static_cast<double>(w);
            viewport.height = static_cast<double>(h);
            viewport.znear = 0.0;
            viewport.zfar = 1.0;
            _renderPassEncoder->setViewport(viewport);
        }
    }

    void MetalGraphicsDevice::setScissor(int x, int y, int w, int h)
    {
        GraphicsDevice::setScissor(x, y, w, h);
        if (_renderPassEncoder && w > 0 && h > 0) {
            MTL::ScissorRect scissor;
            scissor.x = static_cast<NS::UInteger>(x);
            scissor.y = static_cast<NS::UInteger>(y);
            scissor.width = static_cast<NS::UInteger>(w);
            scissor.height = static_cast<NS::UInteger>(h);
            _renderPassEncoder->setScissorRect(scissor);
        }
    }
}
