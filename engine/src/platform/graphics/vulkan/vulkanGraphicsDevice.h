// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan implementation of the graphics device.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <array>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <SDL3/SDL.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "vulkanGpuProfiler.h"
#include "vulkanUniformLayouts.h"

namespace visutwin::canvas
{
    class VulkanRenderPipeline;
    class VulkanRenderTarget;
    class VulkanUniformRingBuffer;

    class VulkanGraphicsDevice : public GraphicsDevice
    {
    public:
        explicit VulkanGraphicsDevice(const GraphicsDeviceOptions& options);
        ~VulkanGraphicsDevice() override;

        // ── Core rendering ───────────────────────────────────────────────
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
        void setReflectionProbeUniforms(Texture* cubemap, const Vector3& boxMin,
            const Vector3& boxMax, bool boxProjection, float intensity,
            float maxLod) override;

        // SSR linearizes the sampled scene depth from the camera clip planes.
        void setCameraClipPlanes(float nearClip, float farClip) override
        {
            _lightingUbo.cameraNearFar[0] = nearClip;
            _lightingUbo.cameraNearFar[1] = farClip;
            _lightingNeedsUpload = true;
        }

        // LTC lookup tables for area lights, bound at set 3 bindings 8/9.
        // Non-owning — the renderer creates them lazily and keeps them alive.
        void setAreaLightLuts(Texture* lut1, Texture* lut2) override
        {
            _areaLightLut1 = lut1;
            _areaLightLut2 = lut2;
        }

        // Clustered spot-shadow atlas (Depth texture2d_array), bound at set 3
        // binding 14. Non-owning — LightTextureAtlas keeps it alive.
        void setClusterShadowAtlas(Texture* atlas) override
        {
            _clusterShadowAtlas = atlas;
        }

        /// Nishita atmosphere parameters (96-byte AtmosphereUniforms block).
        void setAtmosphereUniforms(const void* data, size_t size) override;

        /// Debug surface-quantity output (DebugShaderPass). One compiled variant
        /// covers every mode; the mode rides the lighting UBO as a plain value.
        void setDebugShaderPass(const uint32_t mode) override
        {
            if (_lightingUbo.flagsAndPad[1] != mode) {
                _lightingUbo.flagsAndPad[1] = mode;
                _lightingNeedsUpload = true;
            }
        }

        // ── Shader creation ──────────────────────────────────────────────
        std::shared_ptr<Shader> createShader(const ShaderDefinition& definition,
            const std::string& sourceCode = "") override;

        // ── Resource creation ────────────────────────────────────────────
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture* texture) override;
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            int numVertices, const VertexBufferOptions& options = VertexBufferOptions{}) override;
        std::shared_ptr<VertexBuffer> createVertexBufferFromNativeBuffer(
            const std::shared_ptr<VertexFormat>& format,
            int numVertices, void* nativeBuffer) override;
        bool supportsGpuInstanceCulling() const override { return true; }
        std::unique_ptr<InstanceCuller> createInstanceCuller() override;
        void beginGpuCullBatch() override {}
        void endGpuCullBatch() override { flushUploads(); }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat format, int numIndices,
            const std::vector<uint8_t>& data = {}) override;
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions& options) override;

        // ── Render pass lifecycle ────────────────────────────────────────
        void startRenderPass(RenderPass* renderPass) override;
        void endRenderPass(RenderPass* renderPass) override;

        // ── Dynamic state ────────────────────────────────────────────────
        // Applied immediately to the live command buffer when a render pass
        // is active (mirrors the Metal encoder behaviour); otherwise only the
        // engine-side state is updated and picked up at the next pass start.
        void setViewport(float x, float y, float w, float h) override;
        void setScissor(int x, int y, int w, int h) override;
        void setDepthBias(float depthBias, float slopeScale, float clamp) override;
        void setDynamicBatchPalette(const void* data, size_t size) override;
        void setIndirectDrawBuffer(void* nativeBuffer) override {
            _indirectDrawBuffer = reinterpret_cast<VkBuffer>(nativeBuffer);
        }
        void setMorphState(const std::shared_ptr<VertexBuffer>& deltaBuffer,
            const void* params, size_t paramsSize) override;
        void simulateParticles(const std::shared_ptr<VertexBuffer>& particles,
            const GpuParticleSimParams& params) override;
        void setParticleState(const std::shared_ptr<VertexBuffer>& particles,
            const void* params, size_t paramsSize) override;
        void setGSplatState(const std::shared_ptr<VertexBuffer>& splats,
            const std::shared_ptr<VertexBuffer>& order,
            const std::shared_ptr<VertexBuffer>& sh,
            const void* params, size_t paramsSize) override;
        void setClusterBuffers(const void* lightData, size_t lightSize,
            const void* cellData, size_t cellSize) override;
        void setClusterGridParams(const float* boundsMin, const float* boundsRange,
            const float* cellsCountByBoundsSize, int cellsX, int cellsY, int cellsZ,
            int maxLightsPerCell, int numClusteredLights) override;

        // ── Display management ───────────────────────────────────────────
        void setResolution(int width, int height) override;
        std::pair<int, int> size() const override;

        // False for a frame skipped at acquire: the command buffer is not
        // recording, so every pass this frame is a no-op.
        bool frameRenderable() const override { return _frameActive; }

        // ── Vulkan accessors (for internal use by Vulkan subsystems) ─────
        [[nodiscard]] VkDevice device() const { return _device; }
        [[nodiscard]] VkPhysicalDevice physicalDevice() const { return _physicalDevice; }
        [[nodiscard]] VkQueue graphicsQueue() const { return _graphicsQueue; }
        [[nodiscard]] uint32_t graphicsQueueFamily() const { return _graphicsQueueFamily; }
        [[nodiscard]] VkQueue presentQueue() const { return _presentQueue; }
        [[nodiscard]] uint32_t presentQueueFamily() const { return _presentQueueFamily; }
        [[nodiscard]] VmaAllocator vmaAllocator() const { return _vmaAllocator; }
        [[nodiscard]] VkFormat swapchainFormat() const { return _swapchainFormat; }

        // Commands go to the frame's buffer normally, and to a one-shot buffer
        // while offline work is open (asset-time environment bakes, which run
        // outside the frame loop entirely). Everything that records must go
        // through here rather than reaching for the frame directly.
        [[nodiscard]] VkCommandBuffer currentCommandBuffer() const
        {
            return _offlineCommandBuffer != VK_NULL_HANDLE
                ? _offlineCommandBuffer : _frames[_frameIndex].commandBuffer;
        }

        // True while either a frame or an offline command buffer is recording.
        [[nodiscard]] bool recording() const
        {
            return _frameActive || _offlineCommandBuffer != VK_NULL_HANDLE;
        }

        void beginOfflineWork() override;
        void endOfflineWork() override;
        [[nodiscard]] VkFormat depthFormat() const { return _depthFormat; }
        [[nodiscard]] bool validationEnabled() const { return _validationEnabled; }
        [[nodiscard]] std::shared_ptr<const std::atomic_uint32_t> validationErrorCounter() const {
            return _validationErrorCount;
        }
        // Lightweight diagnostics used by validation/stress tests and runtime
        // telemetry. Values describe the currently recording frame slot.
        [[nodiscard]] size_t currentFrameDescriptorPoolCount() const {
            return _frames[_frameIndex].descriptorPools.size();
        }
        [[nodiscard]] size_t currentFrameCachedImageDescriptorSetCount() const {
            return _frames[_frameIndex].imageDescriptorCache.size();
        }

        // Records resource uploads into a shared batch. flushUploads submits
        // without waiting; retirement callbacks run after its fence signals.
        void enqueueUpload(std::function<void(VkCommandBuffer)> record,
            std::function<void()> retire = {});
        void flushUploads();

        // Queue a GPU-resource destroy to run once every frame that could
        // reference the resource has completed (fence-gated). Resource
        // destructors must use this instead of destroying immediately —
        // an in-flight frame reading a freed image/buffer is a GPU UAF.
        void deferDestroy(std::function<void()> destroyFn);

        // Anisotropic filtering: 1.0 when the device lacks samplerAnisotropy.
        [[nodiscard]] float maxSamplerAnisotropy() const { return _maxSamplerAnisotropy; }

        // True when the optional dualSrcBlend device feature was available and
        // enabled at device creation, so VK_BLEND_FACTOR_SRC1_* may be used.
        [[nodiscard]] bool supportsDualSourceBlending() const override
        {
            return _dualSrcBlendEnabled;
        }

        // Engine-supplied shader source must be GLSL; createShader compiles it
        // with shaderc at runtime and rejects MSL outright.
        [[nodiscard]] ShaderLanguage shaderLanguage() const override
        {
            return ShaderLanguage::Glsl;
        }

        // Expires when the device is destroyed. Resource destructors that can
        // outlive the device (shaders/textures held by static caches torn down
        // at process exit) must lock this before touching VkDevice — a dead
        // device frees its child objects itself, so skipping is correct.
        [[nodiscard]] std::weak_ptr<bool> aliveToken() const { return _aliveToken; }

        // Environment/IBL rendering (vulkanEnvironment.cpp): equirect->cube,
        // reprojection, convolution and the env-atlas bake.
        void renderEnvironment(Texture* target, Texture* sourceEquirect,
            Texture* sourceCubemap, const std::vector<EnvReprojectOp>& ops,
            bool encodeRgbp, bool decodeSrgb, bool clearTarget,
            bool cubemapFaces, bool convolve = false,
            TextureProjection sourceProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT,
            TextureProjection targetProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT);

        void copyRenderTarget(RenderTarget* source, Texture* colorDestination,
            Texture* depthDestination) override;
        void generateMipmaps(Texture* texture) override;

        // A blit needs matching formats, so report what the swapchain actually is.
        PixelFormat backBufferColorFormat() const override
        {
            return _swapchainFormat == VK_FORMAT_R8G8B8A8_UNORM
                ? PixelFormat::PIXELFORMAT_RGBA8 : PixelFormat::PIXELFORMAT_BGRA8;
        }
        void generateEnvConvolve(const EnvConvolvePassParams& params) override;
        void generateEnvAtlas(const EnvAtlasBakeParams& params) override;
        bool supportsCompute() const override { return true; }
        void computeDispatch(const std::vector<Compute*>& computes,
            const std::string& label = "") override;

    private:
        void onFrameStart() override;
        void onFrameEnd() override;

        // Backbuffer capture. recordScreenshotCopy appends a swapchain →
        // host-visible-buffer copy to the frame's command buffer (leaving the
        // image in TRANSFER_SRC for the caller's present transition);
        // writeScreenshotFromBuffer reads it back once the frame fence signals.
        void recordScreenshotCopy(VkCommandBuffer cmd, VkBuffer& outBuffer,
            VmaAllocation& outAllocation);
        void writeScreenshotFromBuffer(VmaAllocation allocation);

        void initialize(const GraphicsDeviceOptions& options);
        // Used only when initialize() throws. The most-derived destructor is
        // not invoked for a failed constructor, so every acquired native handle
        // must be released explicitly.
        void cleanupPartialInitialization() noexcept;

        void initInstance(SDL_Window* window);
        void initDevice();
        [[nodiscard]] bool initSwapchain(
            int width, int height,
            VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
        void cleanupSwapchain();
        void createDepthResources();
        void destroyDepthResources();
        void createPerFrameResources();
        void destroyPerFrameResources();

        // Destroy the four constructor-created samplers, nulling each handle so
        // the call is idempotent. Both teardown paths — the destructor and
        // cleanupPartialInitialization() — go through here: when each kept its
        // own list they drifted, and _materialExtraSampler leaked on every
        // normal device teardown.
        void destroySamplers() noexcept;
        void destroyFallbackImages() noexcept;
        [[nodiscard]] bool createSwapchainSemaphores();
        void destroySwapchainSemaphores();

        // Outcome of a swapchain rebuild attempt.  Deferred is NOT a success:
        // the window has no usable size yet, _swapchainRecreationPending stays
        // set, and no frame can render until a later attempt succeeds.
        enum class SwapchainRecreation { Recreated, Deferred, Failed };

        // Rebuilds the swapchain against oldSwapchain without idling the
        // device. Old resources are retired after the frame fences that can
        // reference them have completed.
        [[nodiscard]] SwapchainRecreation recreateSwapchain();

        // Throttled warning naming the sizes involved, so a device stuck in the
        // Deferred state reports itself instead of silently dropping frames.
        void reportSwapchainDeferred();
        void collectRetiredSwapchains(bool force);

        // A failed queue submission leaves its reset fence unsignaled and its
        // acquire semaphore unconsumed. Submit an empty wait to consume the
        // semaphore, then rebuild the swapchain to release the acquired image.
        void recoverFailedFrameSubmission(VkResult result);

        // Runs queued deferred destroys whose owning frames have provably
        // completed on the GPU (or everything, when force is true — callers
        // must have idled the device first).
        void flushDeferredDestroys(bool force);
        void collectUploads(bool wait);
        void destroyComputeResources();
        bool ensureParticleSimResources();

        // Record current viewport/scissor/depth-bias state into the live
        // command buffer.  Only valid while a render pass is active.
        void applyViewport();
        void applyScissor();
        void applyDepthBias();
        [[nodiscard]] VkDescriptorPool createFrameDescriptorPool(uint32_t maxSets);
        [[nodiscard]] VkDescriptorSet allocateFrameDescriptorSet(
            VkDescriptorSetLayout layout);
        [[nodiscard]] VkDescriptorSet getOrCreateImageDescriptorSet(
            VkDescriptorSetLayout layout,
            std::span<const VkDescriptorImageInfo> imageInfos);
        [[nodiscard]] std::optional<uint32_t> allocateUniform(
            const void* data, VkDeviceSize size);

        SDL_Window* _window = nullptr;
        bool _validationEnabled = false;
        std::shared_ptr<std::atomic_uint32_t> _validationErrorCount =
            std::make_shared<std::atomic_uint32_t>(0);

        // ── Vulkan core objects ──────────────────────────────────────────
        VkInstance _instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;
        VkSurfaceKHR _surface = VK_NULL_HANDLE;
        VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
        VkDevice _device = VK_NULL_HANDLE;
        VkQueue _graphicsQueue = VK_NULL_HANDLE;
        uint32_t _graphicsQueueFamily = 0;
        VkQueue _presentQueue = VK_NULL_HANDLE;
        uint32_t _presentQueueFamily = 0;

        // ── Memory allocator ─────────────────────────────────────────────
        VmaAllocator _vmaAllocator = VK_NULL_HANDLE;

        // ── Swapchain ────────────────────────────────────────────────────
        VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
        VkFormat _swapchainFormat = VK_FORMAT_UNDEFINED;
        // Non-null while offline (out-of-frame) work is recording.
        VkCommandBuffer _offlineCommandBuffer = VK_NULL_HANDLE;
        int _offlineDepth = 0;
        VkExtent2D _swapchainExtent = {0, 0};
        std::vector<VkImage> _swapchainImages;
        std::vector<VkImageView> _swapchainImageViews;
        bool _swapchainRecreationPending = false;
        // Consecutive frames lost to a deferred rebuild; drives the throttle in
        // reportSwapchainDeferred and resets on a successful recreate.
        uint64_t _swapchainDeferredFrames = 0;

        struct RetiredSwapchain
        {
            uint64_t frame = 0;
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            std::vector<VkImageView> imageViews;
            VkImage depthImage = VK_NULL_HANDLE;
            VmaAllocation depthAllocation = VK_NULL_HANDLE;
            VkImageView depthImageView = VK_NULL_HANDLE;
            std::vector<VkSemaphore> renderFinishedSemaphores;
        };
        std::deque<RetiredSwapchain> _retiredSwapchains;

        // ── Depth buffer ─────────────────────────────────────────────────
        VkImage _depthImage = VK_NULL_HANDLE;
        // Tracked layout of the shared backbuffer depth image (like
        // _swapchainImageLayout) so LOAD_OP_LOAD passes keep their contents.
        VkImageLayout _depthImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocation _depthAllocation = VK_NULL_HANDLE;
        VkImageView _depthImageView = VK_NULL_HANDLE;
        VkFormat _depthFormat = VK_FORMAT_D32_SFLOAT;

        // ── Per-frame resources (double-buffered) ────────────────────────
        static constexpr uint32_t kMaxFramesInFlight = 2;

        static constexpr uint32_t kInitialDescriptorSets = 256;
        // Set 3 is the widest image set (15 images + 2 samplers). Must not be less
        // than any set's binding count: getOrCreateImageDescriptorSet returns NULL
        // past this, which silently skips the draw — with no error, so a too-small
        // value looks like "the feature does nothing". Raise it whenever set 3
        // grows; the only cost is descriptor-pool sizing and the cache key width.
        static constexpr uint32_t kMaxCachedImageBindings = 24;

        struct ImageDescriptorKey {
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            uint32_t count = 0;
            std::array<VkSampler, kMaxCachedImageBindings> samplers{};
            std::array<VkImageView, kMaxCachedImageBindings> views{};

            bool operator==(const ImageDescriptorKey&) const = default;
        };

        struct ImageDescriptorKeyHash {
            size_t operator()(const ImageDescriptorKey& key) const;
        };

        struct FrameDescriptorPool {
            VkDescriptorPool handle = VK_NULL_HANDLE;
            uint32_t maxSets = 0;
        };

        struct PerFrame {
            VkCommandPool   commandPool    = VK_NULL_HANDLE;
            VkCommandBuffer commandBuffer  = VK_NULL_HANDLE;
            VkSemaphore     imageAvailable = VK_NULL_HANDLE;
            VkFence         inFlightFence  = VK_NULL_HANDLE;
            std::vector<FrameDescriptorPool> descriptorPools;
            size_t activeDescriptorPool = 0;
            std::unordered_map<ImageDescriptorKey, VkDescriptorSet,
                ImageDescriptorKeyHash> imageDescriptorCache;
        };
        std::array<PerFrame, kMaxFramesInFlight> _frames{};
        uint32_t _frameIndex = 0;
        uint32_t _swapchainImageIndex = 0;

        // True between a successful acquire in onFrameStart and the submit in
        // onFrameEnd. When acquire fails (even after a swapchain rebuild) the
        // frame is skipped: nothing records, submits, or presents.
        bool _frameActive = false;

        // Set when the device or frame lifecycle can no longer safely submit.
        // frameStart/frameEnd become no-ops instead of waiting forever on a
        // fence which cannot be signaled.
        bool _renderingDisabled = false;

        // Fault injection is private and exposed only through the validation
        // smoke test's friend accessor.
        std::optional<VkResult> _submitResultOverride;
        static std::atomic_int _initializationFailureCheckpoint;
        friend struct VulkanGraphicsDeviceTestAccess;

        // Monotonic count of submitted frames — stamps deferred destroys.
        uint64_t _frameNumber = 0;

        struct DeferredDestroy
        {
            uint64_t frame = 0;
            std::function<void()> fn;
        };
        // GPU resources whose owners died while frames may still reference
        // them. Drained in onFrameStart after the fence wait (see
        // flushDeferredDestroys) — destroying immediately would be a GPU
        // use-after-free for any resource used by an in-flight frame.
        std::deque<DeferredDestroy> _deferredDestroys;

        // renderFinished semaphore is one per swapchain image (not per frame
        // in flight): submit signals it, present waits on it, and we can't
        // know which image the next acquire will hand us — so a per-frame
        // slot can collide with a still-pending present.  Per-image, the
        // semaphore's life is tied to the image it gates and validation is
        // valid when submitted through vkQueueSubmit2.
        std::vector<VkSemaphore> _renderFinishedSemaphores;

        // Layout that the currently-acquired swapchain image is in.  Set to
        // UNDEFINED at frame start; flipped to COLOR_ATTACHMENT_OPTIMAL the
        // first time startRenderPass picks the swapchain target; read by
        // onFrameEnd to decide what source layout to use when transitioning
        // to PRESENT_SRC_KHR.  Without this, frames that never reach
        // startRenderPass (e.g. early UI-only frames during asset loading)
        // declare a wrong source layout and validation fails.
        VkImageLayout _swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // ── Upload resources (batched asynchronous staging transfers) ─────
        VkCommandPool _uploadCommandPool = VK_NULL_HANDLE;

        struct PendingUpload
        {
            std::function<void(VkCommandBuffer)> record;
            std::function<void()> retire;
        };
        struct InFlightUpload
        {
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            std::vector<std::function<void()>> retirements;
        };
        std::mutex _uploadMutex;
        std::vector<PendingUpload> _pendingUploads;
        std::deque<InFlightUpload> _inFlightUploads;

        struct ComputePipelineResources {
            VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            std::vector<VkDescriptorType> descriptorTypes;
        };
        std::unordered_map<int, ComputePipelineResources> _computePipelines;
        VkDescriptorSetLayout _particleSimSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout _particleSimPipelineLayout = VK_NULL_HANDLE;
        VkPipeline _particleSimPipeline = VK_NULL_HANDLE;

        std::shared_ptr<VertexBuffer> _pendingParticleBuffer;
        std::array<uint8_t, sizeof(GpuParticleRenderParams)> _pendingParticleParams{};
        size_t _pendingParticleParamsSize = 0;
        std::shared_ptr<VertexBuffer> _pendingGSplatBuffer;
        std::shared_ptr<VertexBuffer> _pendingGSplatOrderBuffer;
        std::shared_ptr<VertexBuffer> _pendingGSplatShBuffer;
        std::array<uint8_t, 160> _pendingGSplatParams{};
        size_t _pendingGSplatParamsSize = 0;

        // ── Render pipeline ──────────────────────────────────────────────
        std::unique_ptr<VulkanRenderPipeline> _renderPipeline;
        VkPipeline _currentPipeline = VK_NULL_HANDLE;
        bool _dynamicRenderingActive = false;

        // Set in startRenderPass when the active RenderPass targets an
        // offscreen RT.  Cleared in endRenderPass.  Used to drive layout
        // transitions back to SHADER_READ_ONLY so subsequent passes can
        // sample the attachments.
        VulkanRenderTarget* _activeOffscreenTarget = nullptr;

        // Extent of the render pass currently being recorded — viewport and
        // scissor fall back to this when the engine state has zero size.
        VkExtent2D _activeExtent{0, 0};

        // True while recording a depth-only offscreen pass (shadow map).  Such
        // passes use a positive-height viewport so the atlas orientation
        // matches the sample matrices (see startRenderPass).
        bool _depthOnlyPass = false;

        // Depth bias state (decals / coplanar overlays).  Pipelines enable
        // depth bias with VK_DYNAMIC_STATE_DEPTH_BIAS, so these are recorded
        // via vkCmdSetDepthBias at pass start and on every setDepthBias call.
        float _depthBiasConstant = 0.0f;
        float _depthBiasSlope = 0.0f;
        float _depthBiasClamp = 0.0f;

        // ── Descriptor pool ──────────────────────────────────────────────
        // One pool per frame-in-flight; reset at frame start (after fence
        // wait) so descriptor sets allocated last frame don't get yanked
        // while still being read by the GPU.

        // ── Push constants ───────────────────────────────────────────────
        struct PushConstants {
            float viewProjection[16]{};
            float model[16]{};
        };
        PushConstants _pushConstants{};
        bool _pushConstantsDirty = false;
        VkBuffer _indirectDrawBuffer = VK_NULL_HANDLE;

        // ── Default resources ────────────────────────────────────────────
        VkSampler _defaultSampler = VK_NULL_HANDLE;
        // Shared by the separate material images (height/detail-normal in the
        // fragment stage, displacement in the vertex stage). Linear + repeat,
        // matching the Metal displacement sampler.
        VkSampler _materialExtraSampler = VK_NULL_HANDLE;
        VkImage _whiteImage = VK_NULL_HANDLE;
        VmaAllocation _whiteAllocation = VK_NULL_HANDLE;
        VkImageView _whiteImageView = VK_NULL_HANDLE;

        // ── Per-draw / per-pass uniform plumbing ─────────────────────────
        // Host-visible ring buffer feeding the dynamic material (set 0) and
        // lighting (set 2) descriptors.  Two persistent descriptor sets point
        // at the ring; only the dynamic offset changes per draw/pass.
        std::unique_ptr<VulkanUniformRingBuffer> _uniformRing;
        VkDescriptorPool _persistentDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet _materialDescriptorSet = VK_NULL_HANDLE;
        VkDescriptorSet _lightingDescriptorSet = VK_NULL_HANDLE;
        VkDeviceSize _uboOffsetAlignment = 256;

        // One-draw geometry state. Palette and params are copied into the
        // fence-gated ring when set; the morph delta buffer remains owned by
        // Morph and is retained until the draw consumes it.
        std::optional<uint32_t> _pendingPaletteOffset;
        VkDeviceSize _pendingPaletteSize = 0;
        std::shared_ptr<VertexBuffer> _pendingMorphDeltaBuffer;
        std::optional<uint32_t> _pendingMorphParamsOffset;
        VkDeviceSize _pendingMorphParamsSize = 0;
        std::optional<uint32_t> _clusterLightOffset;
        VkDeviceSize _clusterLightSize = 0;
        std::optional<uint32_t> _clusterCellOffset;
        VkDeviceSize _clusterCellSize = 0;

        // Packed lighting block; re-uploaded into the ring once per frame (or
        // when setLightingUniforms changes it) and shared by every draw.
        VulkanLightingUBO _lightingUbo{};
        bool _lightingNeedsUpload = true;

        // Once-per-frame throttle for terminal allocation failures. Pool
        // exhaustion itself is recovered by advancing to a larger pool.
        bool _descriptorAllocationErrorWarned = false;
        bool _uniformOverflowWarned = false;

        // Anisotropic-filtering support, resolved at device creation.
        bool _samplerAnisotropyEnabled = false;
        float _maxSamplerAnisotropy = 1.0f;
        bool _dualSrcBlendEnabled = false;
        uint32_t _maxDualSrcDrawBuffers = 0;

        // Dies with the device — see aliveToken().
        std::shared_ptr<bool> _aliveToken = std::make_shared<bool>(true);

        // High-res skybox cubemap bound at set 3 binding 6 (white-cube fallback).
        Texture* _skyboxCubeTexture = nullptr;
        Texture* _reflectionProbeTexture = nullptr;

        // LTC area-light LUTs (set 3 bindings 8/9). Read through _envSampler,
        // which is the linear clamp-to-edge filtering the tables require.
        Texture* _areaLightLut1 = nullptr;
        Texture* _areaLightLut2 = nullptr;

        // Clustered spot-shadow depth array (set 3 binding 14).
        Texture* _clusterShadowAtlas = nullptr;

        // Typed handle for the profiler the base class exposes as _gpuProfiler;
        // null when the device/queue has no timestamp support.
        std::shared_ptr<gpu::VulkanGpuProfiler> _vulkanGpuProfiler;

        uint32_t _lightingSlotOffset = 0;

        // Scene-global environment atlas (equirectangular IBL + skybox source),
        // bound at set 3.  Non-owning — owned by the scene/asset system.  Read
        // through a dedicated clamp-to-edge sampler so the atlas seam doesn't
        // wrap (the per-texture sampler may use REPEAT).
        Texture* _envAtlasTexture = nullptr;
        VkSampler _envSampler = VK_NULL_HANDLE;

        // Directional cascaded shadow map (depth atlas), bound at set 3.  Read
        // through a dedicated clamp sampler.  Non-owning.
        Texture* _shadowMapTexture = nullptr;
        VkSampler _shadowSampler = VK_NULL_HANDLE;

        // Local light shadows (set 3): up to 2 spot-light 2D depth maps and 2
        // omni point-light cubemap depth maps.  Non-owning — owned by the
        // per-light ShadowMap in the renderer.
        Texture* _localShadowTexture0 = nullptr;
        Texture* _localShadowTexture1 = nullptr;
        Texture* _omniShadowCube0 = nullptr;
        Texture* _omniShadowCube1 = nullptr;

        // Light cookies (set 3): up to 2 spot-light 2D cookies and 2 omni
        // cubemap cookies.  Non-owning — the app owns the textures.
        Texture* _cookieTexture2D0 = nullptr;
        Texture* _cookieTexture2D1 = nullptr;
        Texture* _cookieTextureCube0 = nullptr;
        Texture* _cookieTextureCube1 = nullptr;

        // 1×1 white cubemap: fallback for unbound omni shadow slots (a 2D white
        // view cannot back a samplerCube descriptor).
        VkImage _whiteCubeImage = VK_NULL_HANDLE;
        VmaAllocation _whiteCubeAllocation = VK_NULL_HANDLE;
        VkImageView _whiteCubeImageView = VK_NULL_HANDLE;

        // 1×1 single-layer white 2D array: fallback for the clustered shadow
        // atlas slot, for the same reason as the cube above — a VIEW_TYPE_2D
        // view cannot back a texture2DArray descriptor.
        VkImage _whiteArrayImage = VK_NULL_HANDLE;
        VmaAllocation _whiteArrayAllocation = VK_NULL_HANDLE;
        VkImageView _whiteArrayImageView = VK_NULL_HANDLE;

        int _width = 0;
        int _height = 0;
    };
}

#endif // VISUTWIN_HAS_VULKAN
