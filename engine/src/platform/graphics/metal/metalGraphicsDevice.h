// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025.
//
#pragma once

#include <array>
#include <cstdint>
#include <Metal/Metal.hpp>
#include <Foundation/NSAutoreleasePool.hpp>
#include "QuartzCore/CAMetalDrawable.hpp"
#include "QuartzCore/CAMetalLayer.hpp"
#include <SDL3/SDL.h>

#include "metalBindGroupFormat.h"
#include "metalGpuProfiler.h"
#include "metalPaletteRingBuffer.h"
#include "metalTextureBinder.h"
#include "metalUniformBinder.h"
#include "metalUniformRingBuffer.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"

namespace visutwin::canvas
{
    class Compute;
    class MetalCoCPass;
    class MetalComposePass;
    class MetalDepthAwareBlurPass;
    class MetalDofBlurPass;
    class MetalEnvConvolvePass;
    class MetalEnvReprojectPass;
    class MetalEquirectToCubePass;
    class MetalLICPass;
    class MetalMarchingCubesPass;
    class MetalParticleComputePass;
    class MetalSsaoPass;
    class MetalTaaPass;
    class MetalInstanceCullPass;
    class MetalVsmBlurPass;
    class MetalRenderPipeline;
    class MetalComputePipeline;
    class MetalRenderTarget;

    /**
     * Metal implementation of the graphics device.
     * Inherits from GraphicsDevice and implements Metal-specific rendering functionality.
     */
    class MetalGraphicsDevice : public GraphicsDevice
    {
        friend class MetalCoCPass;
        friend class MetalComposePass;
        friend class MetalDepthAwareBlurPass;
        friend class MetalDofBlurPass;
        friend class MetalEnvConvolvePass;
        friend class MetalEnvReprojectPass;
        friend class MetalEquirectToCubePass;
        friend class MetalLICPass;
        friend class MetalMarchingCubesPass;
        friend class MetalParticleComputePass;
        friend class MetalSsaoPass;
        friend class MetalTaaPass;
        friend class MetalInstanceCullPass;
        friend class MetalVsmBlurPass;

    public:
        MetalGraphicsDevice(const GraphicsDeviceOptions& options);
        ~MetalGraphicsDevice();

        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer = nullptr,
            int numInstances = 1, int indirectSlot = -1, bool first = true, bool last = true) override;
        void setTransformUniforms(const Matrix4& viewProjection, const Matrix4& model) override;
        void setLightingUniforms(const Color& ambientColor, const std::vector<GpuLightData>& lights,
            const Vector3& cameraPosition, bool enableNormalMaps, float exposure,
            const FogParams& fogParams = FogParams{}, const ShadowParams& shadowParams = ShadowParams{},
            int toneMapping = 0, const Vector3* ambientSH = nullptr,
            const Matrix4* viewProjection = nullptr) override;
        void setEnvironmentUniforms(Texture* envAtlas, float skyboxIntensity, float skyboxMip,
            const Vector3& skyDomeCenter = Vector3(0,0,0), bool isDome = false,
            Texture* skyboxCubeMap = nullptr) override;

        void setAreaLightLuts(Texture* lut1, Texture* lut2) override
        {
            _areaLightLut1 = lut1;
            _areaLightLut2 = lut2;
        }

        void grabSceneColor(RenderTarget* source) override;
        void setAtmosphereUniforms(const void* data, size_t size) override;

        [[nodiscard]] MTL::Device* raw() const { return _device; }
        [[nodiscard]] MTL::CommandQueue* commandQueue() const { return _commandQueue; }
        [[nodiscard]] CA::MetalDrawable* frameDrawable() const { return _frameDrawable; }

        std::shared_ptr<Shader> createShader(const ShaderDefinition& definition,
            const std::string& sourceCode = "") override;

        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture* texture) override;

        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            int numVertices, const VertexBufferOptions& options = VertexBufferOptions{}) override;

        /// Create a VertexBuffer that adopts a pre-existing MTL::Buffer (zero-copy).
        /// Used for GPU compute output paths where the buffer is already filled.
        std::shared_ptr<VertexBuffer> createVertexBufferFromMTLBuffer(
            const std::shared_ptr<VertexFormat>& format,
            int numVertices, MTL::Buffer* externalBuffer);

        std::shared_ptr<VertexBuffer> createVertexBufferFromNativeBuffer(
            const std::shared_ptr<VertexFormat>& format,
            int numVertices, void* nativeBuffer) override;

        bool supportsGpuInstanceCulling() const override { return true; }
        std::unique_ptr<InstanceCuller> createInstanceCuller() override;

        // One command buffer + one waitUntilCompleted for ALL cull dispatches
        // between begin/end (previously each MeshInstance's cull committed and
        // waited on its own command buffer — a CPU-GPU round trip per instance).
        void beginGpuCullBatch() override;
        void endGpuCullBatch() override;
        [[nodiscard]] MTL::CommandBuffer* gpuCullBatchCommandBuffer() const { return _gpuCullBatchCommandBuffer; }

        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat format, int numIndices,
            const std::vector<uint8_t>& data = {}) override;
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions& options) override;
        void executeComposePass(const ComposePassParams& params) override;
        void executeTAAPass(Texture* sourceTexture, Texture* historyTexture, Texture* depthTexture,
            const Matrix4& viewProjectionPrevious, const Matrix4& viewProjectionInverse,
            const std::array<float, 4>& jitters, const std::array<float, 4>& cameraParams,
            bool highQuality, bool historyValid) override;
        void executeSsaoPass(const SsaoPassParams& params) override;
        void executeCoCPass(const CoCPassParams& params) override;
        void executeDofBlurPass(const DofBlurPassParams& params) override;
        void executeDepthAwareBlurPass(const DepthAwareBlurPassParams& params, bool horizontal) override;
        void executeVsmBlurPass(const VsmBlurPassParams& params, bool horizontal) override;
        void generateEnvReproject(const EnvReprojectPassParams& params) override;
        void generateEnvConvolve(const EnvConvolvePassParams& params) override;
        void generateEnvAtlas(const EnvAtlasBakeParams& params) override;
        void generateEquirectToCubemap(const EquirectToCubeParams& params) override;
        bool supportsCompute() const override { return true; }
        void computeDispatch(const std::vector<Compute*>& computes, const std::string& label = "") override;

        std::pair<int, int> size() const override;

        void setDepthBias(float depthBias, float slopeScale, float clamp) override;

        void startRenderPass(RenderPass* renderPass) override;

        void endRenderPass(RenderPass* renderPass) override;

        void setResolution(int width, int height) override;
        void setViewport(float x, float y, float w, float h) override;
        void setScissor(int x, int y, int w, int h) override;

        /// Set the indirect draw buffer for the next draw call.
        /// The buffer is consumed (reset to nullptr) after one indirect draw.
        void setIndirectDrawBuffer(void* nativeBuffer) override;

        /// Bind the dynamic batch matrix palette at slot 6 via setVertexBytes.
        /// Uses Metal buffer for bone data.
        void setDynamicBatchPalette(const void* data, size_t size) override;

        /// Bind morph target delta buffer (vertex slot 9) + params (vertex slot 10)
        /// for the next draw call.
        void setMorphState(const std::shared_ptr<VertexBuffer>& deltaBuffer,
            const void* params, size_t paramsSize) override;

        /// Bind Gaussian splat buffers (vertex slots 7/8) + params (vertex slot 11)
        /// for the next draw call.
        void setGSplatState(const std::shared_ptr<VertexBuffer>& splats,
            const std::shared_ptr<VertexBuffer>& order, const void* params, size_t paramsSize) override;

        /// Bind clustered lighting data for the current frame.
        /// Allocates/grows internal MTL::Buffers and copies data.
        void setClusterBuffers(const void* lightData, size_t lightSize,
            const void* cellData, size_t cellSize) override;

        void setClusterGridParams(const float* boundsMin, const float* boundsRange,
            const float* cellsCountByBoundsSize,
            int cellsX, int cellsY, int cellsZ, int maxLightsPerCell,
            int numClusteredLights) override;

    private:
        void onFrameStart() override;
        void onFrameEnd() override;

        int submitVertexBuffer(const std::shared_ptr<VertexBuffer>& vertexBuffer, int slot);

        // Sentinel null shared_ptr used as a const-ref return for empty VB slots,
        // avoiding shared_ptr copy when checking _vertexBuffers boundaries.
        static inline const std::shared_ptr<VertexBuffer> _nullVertexBuffer{nullptr};

        SDL_Window* _window;

        MTL::Device* _device;
        bool _ownsDevice = false;
        MTL::CommandQueue* _commandQueue;

        CA::MetalLayer* _metalLayer;

        // Pass encoder - can be render or compute pass encoder
        union {
            MTL::RenderCommandEncoder* _renderPassEncoder;
            MTL::ComputeCommandEncoder* _computePassEncoder;
        };

        // Active command buffer / drawable for the current pass.
        CA::MetalDrawable* _currentDrawable = nullptr;
        MTL::CommandBuffer* _commandBuffer = nullptr;

        // Cached drawable for the current frame. Multiple back-buffer render passes
        // within a single frame must share the same drawable (Metal's nextDrawable()
        // returns a different drawable each call, unlike WebGL's persistent back buffer).
        CA::MetalDrawable* _frameDrawable = nullptr;

        MTL::RenderPipelineState* _pipelineState = nullptr;
        MTL::Buffer* _indirectDrawBuffer = nullptr;  // Set by setIndirectDrawBuffer(), consumed by draw()

        // Live between beginGpuCullBatch/endGpuCullBatch (autoreleased).
        MTL::CommandBuffer* _gpuCullBatchCommandBuffer = nullptr;

        // Dynamic batch palette: ring-buffer offset for slot 6.
        // Set by setDynamicBatchPalette() → allocate from _paletteRing,
        // consumed (reset to SIZE_MAX) after draw() → setVertexBufferOffset().
        size_t _pendingPaletteOffset = SIZE_MAX;

        // Morph state: set by setMorphState(), consumed (buffer reset) by draw().
        // Delta buffer binds at vertex slot 9, params via setVertexBytes at slot 10.
        MTL::Buffer* _pendingMorphDeltaBuffer = nullptr;
        std::array<uint8_t, 128> _pendingMorphParams{};
        size_t _pendingMorphParamsSize = 0;

        // GPU pass profiler (nullptr when unsupported). Also stored in the base
        // class _gpuProfiler for the public accessor.
        std::shared_ptr<gpu::MetalGpuProfiler> _metalGpuProfiler;

        // Gaussian splat state: set by setGSplatState(), consumed by draw().
        // Splats bind at vertex slot 7, order at 8, params bytes at 11.
        MTL::Buffer* _pendingGSplatBuffer = nullptr;
        MTL::Buffer* _pendingGSplatOrderBuffer = nullptr;
        std::array<uint8_t, 192> _pendingGSplatParams{};
        size_t _pendingGSplatParamsSize = 0;
        MTL::SamplerState* _defaultSampler = nullptr;
        // Clamp-to-edge sampler for screen-space post passes (no mips/aniso) —
        // the repeat-mode default sampler wraps kernel taps at frame borders.
        MTL::SamplerState* _postSampler = nullptr;
        MTL::DepthStencilState* _defaultDepthStencilState = nullptr;
        MTL::DepthStencilState* _noWriteDepthStencilState = nullptr;
        MTL::Texture* _backBufferDepthTexture = nullptr;
        int _backBufferDepthWidth = 0;
        int _backBufferDepthHeight = 0;

        std::unique_ptr<MetalRenderPipeline> _renderPipeline;
        std::unique_ptr<MetalComputePipeline> _computePipeline;

        std::vector<std::shared_ptr<MetalBindGroupFormat>> _bindGroupFormats;
        std::unique_ptr<MetalCoCPass> _cocPass;
        std::unique_ptr<MetalComposePass> _composePass;
        std::unique_ptr<MetalDofBlurPass> _dofBlurPass;
        std::unique_ptr<MetalSsaoPass> _ssaoPass;
        std::unique_ptr<MetalDepthAwareBlurPass> _blurPassH;
        std::unique_ptr<MetalDepthAwareBlurPass> _blurPassV;
        std::unique_ptr<MetalVsmBlurPass> _vsmBlurPassH;
        std::unique_ptr<MetalVsmBlurPass> _vsmBlurPassV;
        std::unique_ptr<MetalTaaPass> _taaPass;
        std::unique_ptr<MetalEnvReprojectPass> _envReprojectPass;
        std::unique_ptr<MetalEnvConvolvePass> _envConvolvePass;
        std::unique_ptr<MetalEquirectToCubePass> _equirectToCubePass;

        // Triple-buffered ring buffers for per-draw uniform data.
        // Replaces setVertexBytes()/setFragmentBytes() with pre-allocated MTLBuffer
        // + setVertexBufferOffset() for significantly reduced CPU overhead at scale.
        std::unique_ptr<MetalUniformRingBuffer> _transformRing;  // ModelData (slot 2)
        std::unique_ptr<MetalUniformRingBuffer> _uniformRing;    // MaterialUniforms (slot 3) + LightingUniforms (slot 4)
        std::unique_ptr<MetalPaletteRingBuffer> _paletteRing;   // Dynamic batch palette (slot 6)

        // Uniform packing, ring-buffer allocation, and per-pass deduplication.
        MetalUniformBinder _uniformBinder;

        // Per-pass texture binding deduplication (slots 0-8 + sampler).
        MetalTextureBinder _textureBinder;

        // LTC area-light lookup textures (slots 20/21), owned by the renderer.
        Texture* _areaLightLut1 = nullptr;
        Texture* _areaLightLut2 = nullptr;

        // Scene color grab target (dynamic refraction): full-mip copy of the scene
        // color made by the depth-layer grab pass, wrapped for slot-22 binding.
        MTL::Texture* _sceneGrabRaw = nullptr;
        std::shared_ptr<Texture> _sceneGrabWrapper;

        // Clustered lighting GPU buffers (fragment slots 7 and 8).
        MTL::Buffer* _clusterLightBuffer = nullptr;
        MTL::Buffer* _clusterCellBuffer = nullptr;
        size_t _clusterLightBufferCapacity = 0;
        size_t _clusterCellBufferCapacity = 0;
        bool _clusterBuffersSet = false;

        // Per-frame autorelease pool.  Metal-cpp methods like commandBuffer()
        // return autoreleased objects that accumulate until a pool drains.
        // Without per-frame draining, memory grows without bound during
        // continuous rendering (observed as 25 GB leak in long sessions).
        NS::AutoreleasePool* _framePool = nullptr;
    };
}
