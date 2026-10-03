// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025
//
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>
#include <Metal/Metal.hpp>
#include <Foundation/NSAutoreleasePool.hpp>
#include "QuartzCore/CAMetalDrawable.hpp"
#include "QuartzCore/CAMetalLayer.hpp"
#include <SDL3/SDL.h>

#include "metalBindGroupFormat.h"
#include "metalGpuProfiler.h"
#include "metalFrameGate.h"
#include "metalPaletteRingBuffer.h"
#include "metalSamplerCache.h"
#include "metalTextureBinder.h"
#include "metalUniformBinder.h"
#include "metalUniformRingBuffer.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"

namespace visutwin::canvas
{
    class Compute;
    class MetalMarchingCubesPass;
    class MetalRenderPipeline;
    class MetalComputePipeline;
    class MetalRenderTarget;

    /**
     * Metal implementation of the graphics device.
     * Inherits from GraphicsDevice and implements Metal-specific rendering functionality.
     */
    class MetalGraphicsDevice : public GraphicsDevice
    {
        friend class MetalMarchingCubesPass;

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
            const Matrix4* viewProjection = nullptr, uint32_t meshLightMask = MASK_AFFECT_DYNAMIC) override;
        void setReflectionProbeUniforms(Texture* cubemap, const Vector3& boxMin,
            const Vector3& boxMax, bool boxProjection, float intensity, float maxLod) override;
        void setCameraClipPlanes(float nearClip, float farClip) override;
        void setDebugShaderPass(uint32_t mode) override;
        void setEnvironmentUniforms(Texture* envAtlas, float skyboxIntensity, float skyboxMip,
            const Vector3& skyDomeCenter = Vector3(0,0,0), bool isDome = false,
            Texture* skyboxCubeMap = nullptr) override;
        void setSkyboxRotation(const Quaternion& rotation) override { _uniformBinder.setSkyboxRotation(rotation); }
        void setDitherJitter(const Vector4& jitter) override { _uniformBinder.setDitherJitter(jitter); }

        void setAreaLightLuts(Texture* lut1, Texture* lut2) override
        {
            _areaLightLut1 = lut1;
            _areaLightLut2 = lut2;
        }

        void setClusterShadowAtlas(Texture* atlas) override
        {
            _clusterShadowAtlas = atlas;
        }

        void setClusterCookieAtlas(Texture* atlas) override
        {
            _clusterCookieAtlas = atlas;
        }

        void copyRenderTarget(RenderTarget* source, Texture* colorDestination,
            Texture* depthDestination) override;
        void generateMipmaps(Texture* texture) override;

        /// Depth-stencil, for UI masks (metal::kBackBufferDepthFormat); a blit needs matching
        /// formats, so a depth grab from the back buffer allocates this.
        PixelFormat backBufferDepthFormat() const override
        {
            return PixelFormat::PIXELFORMAT_DEPTHSTENCIL;
        }
        // The CAMetalLayer is created BGRA8Unorm, and a blit needs matching formats.
        PixelFormat backBufferColorFormat() const override
        {
            return PixelFormat::PIXELFORMAT_BGRA8;
        }
        void setAtmosphereUniforms(const void* data, size_t size) override;

        [[nodiscard]] MTL::Device* raw() const { return _device; }
        [[nodiscard]] MTL::CommandQueue* commandQueue() const { return _commandQueue; }
        [[nodiscard]] CA::MetalDrawable* frameDrawable() const { return _frameDrawable; }

        /**
         * The ONE command buffer the device is encoding into. Render passes, in-frame
         * copies and mipmap generation, compute dispatches and the culling batch all
         * encode into it, in the order they are issued, and it is committed when the
         * frame ends — not a buffer per pass and per dispatch. A buffer costs a creation
         * and a commit (about as much CPU as fifty draws), a frame has dozens of passes,
         * and the queue hands out only 64 buffers at a time: the 65th `commandBuffer()`
         * BLOCKS until the GPU finishes one, which a scene with a few hundred particle
         * emitters ran into every frame.
         *
         * Returns the open buffer, creating it if there is none, or null while an
         * encoder is open on it (Metal allows one at a time). Do not commit it: call
         * flushCommands(). A compute dispatch issued outside a frame stays in it until
         * the next frame ends (or a flush), as the Vulkan backend queues one for its
         * frame; the other operations commit it themselves when no frame is open.
         *
         * Code that makes its own buffer from commandQueue() and commits it runs BEFORE
         * whatever is still in the open buffer. That is right for work nothing in the
         * frame feeds (a texture upload, a kernel on its own data) and wrong for
         * anything that reads what the frame has rendered: that has to flush first, as
         * the texture readback does.
         */
        [[nodiscard]] MTL::CommandBuffer* openCommandBuffer();

        /**
         * Commits the open command buffer, so the GPU starts on it and any buffer
         * committed afterwards is ordered behind it. Called when a frame ends, after a
         * pass that encoded a lot of work (the GPU should not sit idle while the rest of
         * the frame is encoded), and before anything that waits for or reads back GPU
         * results. False, with nothing committed, while an encoder is open.
         */
        bool flushCommands();

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


        // Dual-source blending is core Metal — supported on every device that runs this backend.
        bool supportsDualSourceBlending() const override { return true; }

        /// ASTC on the Apple GPU families, BC where the device reports it (every Mac,
        /// Apple silicon included); nothing else is block-compressed here.
        bool supportsCompressedFormat(PixelFormat format) const override;

        // Encode all per-frame cull dispatches into one asynchronously submitted
        // command buffer. Rendering on the same queue consumes the results later.
        void beginOfflineWork() override;
        void endOfflineWork() override;

        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat format, int numIndices,
            const std::vector<uint8_t>& data = {}) override;
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions& options) override;
        bool supportsCompute() const override { return true; }
        void computeDispatch(const std::vector<Compute*>& computes, const std::string& label = "") override;

        /// A shared-storage MTLBuffer write lands directly in the memory the GPU
        /// reads, and the ring semaphores let the CPU run this many frames ahead,
        /// so a per-frame CPU-written buffer needs this many copies to cycle through.
        int maxFramesInFlight() const override { return MetalUniformRingBuffer::kMaxInflightFrames; }

        std::pair<int, int> size() const override;
        float devicePixelRatio() const override;
        std::pair<int, int> windowSizeInPoints() const override;

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
        void setDynamicBatchPalette(const void* data, size_t size, uint64_t contentVersion = 0) override;

        /// Bind morph target delta buffer (vertex slot 9) + params (vertex slot 10)
        /// for the next draw call.
        void setMorphState(const std::shared_ptr<VertexBuffer>& deltaBuffer,
            const void* params, size_t paramsSize) override;

        /// Bind Gaussian splat buffers (vertex slots 7/8) + params (vertex slot 11)
        /// for the next draw call.
        using GraphicsDevice::setParticleState;
        void setParticleState(const std::shared_ptr<VertexBuffer>& particles,
            const std::shared_ptr<VertexBuffer>& order, const std::shared_ptr<VertexBuffer>& meshVertices,
            const void* params, size_t paramsSize) override;
        void setGSplatState(const std::shared_ptr<VertexBuffer>& splats,
            const std::shared_ptr<VertexBuffer>& order, const std::shared_ptr<VertexBuffer>& sh,
            const void* params, size_t paramsSize) override;

        /// Bind clustered lighting data for the current frame.
        /// Allocates/grows internal MTL::Buffers and copies data.
        void setClusterBuffers(const void* lightData, size_t lightSize,
            const void* cellData, size_t cellSize) override;

        void setClusterGridParams(const float* boundsMin, const float* boundsRange,
            const float* cellsCountByBoundsSize,
            int cellsX, int cellsY, int cellsZ, int maxLightsPerCell,
            int numClusteredLights) override;

    private:
        struct DepthStencilCacheKey
        {
            uint32_t frontState;
            uint32_t backState;
            bool depthTest;
            bool depthWrite;
            CompareFunction depthFunc;

            bool operator==(const DepthStencilCacheKey&) const = default;
        };

        struct DepthStencilCacheKeyHash
        {
            size_t operator()(const DepthStencilCacheKey& key) const noexcept;
        };

        void onFrameStart() override;
        void waitForNextFrame() override;
        void onFrameEnd() override;

        // Blits the finished drawable into a shared-storage staging texture and
        // writes it out as a PNG. Blocks until the copy completes.
        void captureDrawable(CA::MetalDrawable* drawable);

        int submitVertexBuffer(const std::shared_ptr<VertexBuffer>& vertexBuffer, int slot);
        MTL::DepthStencilState* resolveDepthStencilState(const DepthState* depthState,
            StencilParameters* stencilFront, StencilParameters* stencilBack);

        // startRenderPass()'s stages that touch device state; the attachment setup
        // is free functions in metalGraphicsDevice.cpp.

        /// The frame's drawable, acquired on its first back-buffer pass and reused by
        /// the rest; false when none could be had.
        bool acquirePassDrawable();
        /// (Re)creates the back buffer's depth-stencil texture at the drawable's size.
        void ensureBackBufferDepthTexture(int width, int height);
        /// Names the encoder and opens a debug group for frame captures and traces.
        void labelPassEncoder(RenderPass& renderPass);
        /// The state every new encoder starts from: cleared caches, default depth
        /// state and winding, the full-target viewport, and the per-pass buffers.
        void beginPassEncoderState(const RenderTarget* target);

        // draw()'s stages, in the order it runs them.

        struct IndexBinding
        {
            MTL::Buffer* buffer = nullptr;   // null for a non-indexed draw
            MTL::IndexType type = MTL::IndexTypeUInt16;
        };
        /// The bytes that go in the per-draw material slot.
        struct UniformBlock
        {
            const void* data = nullptr;
            size_t size = 0;
        };
        /// Submits the vertex and instance buffers and sets the pipeline for the bound
        /// state; false when no pipeline could be made and the draw must be skipped.
        bool bindDrawPipeline(MTL::RenderCommandEncoder* passEncoder, const Primitive& primitive,
            const std::shared_ptr<IndexBuffer>& indexBuffer);
        /// The native buffer and index type; nullopt when the draw must be skipped.
        std::optional<IndexBinding> resolveIndexBinding(const std::shared_ptr<IndexBuffer>& indexBuffer);
        void applyEncoderCullMode(MTL::RenderCommandEncoder* passEncoder);
        /// Binds the material's textures when they changed and returns its uniform block.
        UniformBlock bindMaterialTextures(MTL::RenderCommandEncoder* passEncoder, const Material* boundMaterial);
        /// A quad pass's inputs, or the scene-wide textures (shadows, env, cookies ...).
        void bindPassTextures(MTL::RenderCommandEncoder* passEncoder);
        void updateReflectionUniforms();
        void submitDrawUniforms(MTL::RenderCommandEncoder* passEncoder, const Material* boundMaterial,
            const UniformBlock& uniforms);
        void bindDrawSampler(MTL::RenderCommandEncoder* passEncoder);
        void applyDepthStencilState(MTL::RenderCommandEncoder* passEncoder);
        /// The one-shot palette, morph, particle and splat bindings, consumed here.
        void bindPendingDrawResources(MTL::RenderCommandEncoder* passEncoder);
        void encodeDraw(MTL::RenderCommandEncoder* passEncoder, const Primitive& primitive,
            const IndexBinding& index, int numInstances, int indirectSlot);

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
        // Taken by waitForNextFrame before the frame starts: the frame gate's slot and a
        // drawable (retained, adopted by the frame's autorelease pool in onFrameStart).
        bool _frameSlotTaken = false;
        CA::MetalDrawable* _preparedDrawable = nullptr;

        // What the CURRENT render encoder holds, so a draw re-issues only the state that
        // differs from the previous draw's. draw() is the only writer of each of these on
        // the encoder; startRenderPass resets them all with the new encoder
        // (resetEncoderStateCache). Re-issuing the pipeline, the vertex buffer, the winding,
        // the cull mode and the depth-stencil state on every draw makes the driver re-emit
        // its render state for each, a large share of the frame's CPU at high draw counts.
        MTL::RenderPipelineState* _pipelineState = nullptr;   // non-owning, see draw()
        MTL::Buffer* _encoderVertexBuffer0 = nullptr;
        MTL::Buffer* _encoderInstancingBuffer = nullptr;
        MTL::DepthStencilState* _encoderDepthStencilState = nullptr;
        int _encoderCullMode = -1;
        bool _encoderStencilReferenceValid = false;
        uint32_t _encoderStencilReferenceFront = 0;
        uint32_t _encoderStencilReferenceBack = 0;
        void resetEncoderStateCache();

        MTL::Buffer* _indirectDrawBuffer = nullptr;  // Set by setIndirectDrawBuffer(), consumed by draw()


        // Nesting depth of beginOfflineWork/endOfflineWork.
        int _envBatchDepth = 0;

        // openCommandBuffer(): retained while open, released when committed.
        MTL::CommandBuffer* _openCommandBuffer = nullptr;
        // Between onFrameStart and onFrameEnd. Outside a frame nothing will commit the
        // open buffer by itself, so the scopes that end there flush it.
        bool _insideFrame = false;
        // False until the first onFrameStart: the per-draw rings have no frame region of
        // their own yet, and work encoded now borrows region 0 (see endOfflineWork).
        bool _frameEverStarted = false;
        // What the open buffer holds, for the early commit after a heavy pass.
        uint32_t _openBufferDraws = 0;
        uint64_t _openBufferVertices = 0;

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
        // Splats bind at vertex slot 7, order at 8, SH coeffs at 12, params bytes at 11.
        MTL::Buffer* _pendingGSplatShBuffer = nullptr;
        MTL::Buffer* _pendingGSplatBuffer = nullptr;
        MTL::Buffer* _pendingGSplatOrderBuffer = nullptr;
        std::array<uint8_t, kGSplatParamsCapacity> _pendingGSplatParams{};
        size_t _pendingGSplatParamsSize = 0;

        // GPU particle emitters: sim compute pipeline (lazy) + per-draw binding.
        MTL::Buffer* _pendingParticleBuffer = nullptr;
        MTL::Buffer* _pendingParticleOrderBuffer = nullptr;
        MTL::Buffer* _pendingParticleMeshBuffer = nullptr;
        std::array<uint8_t, sizeof(GpuParticleRenderParams)> _pendingParticleParams{};
        size_t _pendingParticleParamsSize = 0;
        // Material sampler states, keyed on each texture's own wrap and filter. It owns
        // the default sampler below too, which is borrowed from it.
        MetalSamplerCache _samplerCache;
        // Repeat, trilinear, anisotropic: slot 0 of every non-quad draw, and every material
        // sampler slot whose map is absent or in the default state. Owned by _samplerCache.
        MTL::SamplerState* _defaultSampler = nullptr;
        // Clamp-to-edge sampler for screen-space post passes (no mips/aniso) —
        // the repeat-mode default sampler wraps kernel taps at frame borders.
        MTL::SamplerState* _postSampler = nullptr;
        MTL::DepthStencilState* _defaultDepthStencilState = nullptr;
        MTL::DepthStencilState* _noWriteDepthStencilState = nullptr;
        MTL::DepthStencilState* _noTestDepthStencilState = nullptr;
        MTL::DepthStencilState* _noTestNoWriteDepthStencilState = nullptr;
        std::unordered_map<DepthStencilCacheKey, MTL::DepthStencilState*,
            DepthStencilCacheKeyHash> _stencilStateCache;
        MTL::Texture* _backBufferDepthTexture = nullptr;
        int _backBufferDepthWidth = 0;
        int _backBufferDepthHeight = 0;

        std::unique_ptr<MetalRenderPipeline> _renderPipeline;

        // One-entry memo for the bound target's attachment-format key. Resolving it
        // needs a dynamic_cast (the back buffer is a plain RenderTarget), which used
        // to run for every draw call; the target cannot change within a pass, so it
        // is resolved once per pass instead. Reset in startRenderPass so a freed
        // target cannot be matched by a new one reusing its address.
        const RenderTarget* _psoFormatKeyTarget = nullptr;
        uint32_t _psoFormatKey = 0;
        bool _psoFormatKeyValid = false;

        /// Attachment-format key of the currently bound render target (0 = back buffer).
        uint32_t renderTargetFormatKey();
        std::unique_ptr<MetalComputePipeline> _computePipeline;

        std::vector<std::shared_ptr<MetalBindGroupFormat>> _bindGroupFormats;

        // Paces the CPU against the GPU; the rings below, and the cluster buffer sets,
        // rely on it. Declared before them, which hold a reference to it.
        MetalFrameGate _frameGate;

        // Triple-buffered ring buffers for per-draw uniform data.
        // Replaces setVertexBytes()/setFragmentBytes() with pre-allocated MTLBuffer
        // + setVertexBufferOffset() for significantly reduced CPU overhead at scale.
        std::unique_ptr<MetalUniformRingBuffer> _transformRing;  // ModelData (slot 2)
        bool _encoderDebugGroupOpen = false;   // pass-name debug group on the render encoder
        std::unique_ptr<MetalUniformRingBuffer> _uniformRing;    // MaterialUniforms (slot 3) + LightingUniforms (slot 4)
        std::unique_ptr<MetalPaletteRingBuffer> _paletteRing;   // Dynamic batch palette (slot 6)

        // Uniform packing, ring-buffer allocation, and per-pass deduplication.
        MetalUniformBinder _uniformBinder;

        // Per-pass texture binding deduplication (slots 0-8 + sampler).
        MetalTextureBinder _textureBinder;
        // The instance lightmap the material slots were last bound with, so a draw
        // that keeps the material but changes it still rebinds (see draw()).
        Texture* _boundInstanceLightMap = nullptr;

        // LTC area-light lookup textures (slots 20/21), owned by the renderer.
        Texture* _areaLightLut1 = nullptr;
        Texture* _areaLightLut2 = nullptr;
        Texture* _clusterShadowAtlas = nullptr;
        Texture* _clusterCookieAtlas = nullptr;   // clustered cookie atlas (slot 36)  // clustered spot-shadow depth array (slot 26)

        // Scene color grab target (dynamic refraction): full-mip copy of the scene
        // color made by the depth-layer grab pass, wrapped for slot-22 binding.

        // Clustered lighting GPU buffers (fragment slots 7 and 8).
        // Attachment size of the open render pass, which the scissor is clamped to.
        int _passWidth = 0;
        int _passHeight = 0;

        // A cluster grid's light and cell buffers are CPU-written and GPU-read, so each
        // grid a frame binds gets its own pair, and the pairs of one frame are not
        // written again until that frame's ring region comes round (see
        // setClusterBuffers). The two pointers are the pair bound now, owned by the sets.
        struct ClusterBufferSet
        {
            MTL::Buffer* light = nullptr;
            MTL::Buffer* cell = nullptr;
            size_t lightCapacity = 0;
            size_t cellCapacity = 0;
        };
        std::array<std::vector<ClusterBufferSet>, MetalUniformRingBuffer::kMaxInflightFrames> _clusterBufferSets;
        size_t _clusterFrameSlot = 0;
        size_t _clusterSetsUsed = 0;
        const void* _clusterLastLightData = nullptr;
        const void* _clusterLastCellData = nullptr;
        MTL::Buffer* _clusterLightBuffer = nullptr;
        MTL::Buffer* _clusterCellBuffer = nullptr;
        bool _clusterBuffersSet = false;
        size_t clusterBufferVram() const;

        // Per-frame autorelease pool.  Metal-cpp methods like commandBuffer()
        // return autoreleased objects that accumulate until a pool drains.
        // Without per-frame draining, memory grows without bound during
        // continuous rendering (observed as 25 GB leak in long sessions).
        NS::AutoreleasePool* _framePool = nullptr;
    };
}
