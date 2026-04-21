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
    class MetalEnvReprojectPass;
    class MetalLICPass;
    class MetalMarchingCubesPass;
    class MetalParticleComputePass;
    class MetalSsaoPass;
    class MetalTaaPass;
    class MetalInstanceCullPass;
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
        friend class MetalEnvReprojectPass;
        friend class MetalLICPass;
        friend class MetalMarchingCubesPass;
        friend class MetalParticleComputePass;
        friend class MetalSsaoPass;
        friend class MetalTaaPass;
        friend class MetalInstanceCullPass;

    public:
        MetalGraphicsDevice(const GraphicsDeviceOptions& options);
        ~MetalGraphicsDevice();

        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer = nullptr,
            int numInstances = 1, int indirectSlot = -1, bool first = true, bool last = true) override;
        void setTransformUniforms(const Matrix4& viewProjection, const Matrix4& model) override;
        void setLightingUniforms(const Color& ambientColor, const std::vector<GpuLightData>& lights,
            const Vector3& cameraPosition, bool enableNormalMaps, float exposure,
            const FogParams& fogParams = FogParams{}, const ShadowParams& shadowParams = ShadowParams{},
            int toneMapping = 0) override;
        void setEnvironmentUniforms(Texture* envAtlas, float skyboxIntensity, float skyboxMip,
            const Vector3& skyDomeCenter = Vector3(0,0,0), bool isDome = false,
            Texture* skyboxCubeMap = nullptr) override;
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
        void generateEnvReproject(const EnvReprojectPassParams& params) override;
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

        // Dynamic batch palette: ring-buffer offset for slot 6.
        // Set by setDynamicBatchPalette() → allocate from _paletteRing,
        // consumed (reset to SIZE_MAX) after draw() → setVertexBufferOffset().
        size_t _pendingPaletteOffset = SIZE_MAX;
        MTL::SamplerState* _defaultSampler = nullptr;
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
        std::unique_ptr<MetalTaaPass> _taaPass;
        std::unique_ptr<MetalEnvReprojectPass> _envReprojectPass;

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
