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

#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
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

        // ── Shader creation ──────────────────────────────────────────────
        std::shared_ptr<Shader> createShader(const ShaderDefinition& definition,
            const std::string& sourceCode = "") override;

        // ── Resource creation ────────────────────────────────────────────
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture* texture) override;
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            int numVertices, const VertexBufferOptions& options = VertexBufferOptions{}) override;
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

        // ── Display management ───────────────────────────────────────────
        void setResolution(int width, int height) override;
        std::pair<int, int> size() const override;

        // ── Vulkan accessors (for internal use by Vulkan subsystems) ─────
        [[nodiscard]] VkDevice device() const { return _device; }
        [[nodiscard]] VkPhysicalDevice physicalDevice() const { return _physicalDevice; }
        [[nodiscard]] VkQueue graphicsQueue() const { return _graphicsQueue; }
        [[nodiscard]] uint32_t graphicsQueueFamily() const { return _graphicsQueueFamily; }
        [[nodiscard]] VmaAllocator vmaAllocator() const { return _vmaAllocator; }
        [[nodiscard]] VkFormat swapchainFormat() const { return _swapchainFormat; }
        [[nodiscard]] VkFormat depthFormat() const { return _depthFormat; }
        [[nodiscard]] VkDescriptorPool descriptorPool() const {
            return _frames[_frameIndex].descriptorPool;
        }

        // Upload command pool for immediate staging transfers
        [[nodiscard]] VkCommandPool uploadCommandPool() const { return _uploadCommandPool; }
        [[nodiscard]] VkFence uploadFence() const { return _uploadFence; }

        // Queue a GPU-resource destroy to run once every frame that could
        // reference the resource has completed (fence-gated). Resource
        // destructors must use this instead of destroying immediately —
        // an in-flight frame reading a freed image/buffer is a GPU UAF.
        void deferDestroy(std::function<void()> destroyFn);

        // Anisotropic filtering: 1.0 when the device lacks samplerAnisotropy.
        [[nodiscard]] float maxSamplerAnisotropy() const { return _maxSamplerAnisotropy; }

        // Expires when the device is destroyed. Resource destructors that can
        // outlive the device (shaders/textures held by static caches torn down
        // at process exit) must lock this before touching VkDevice — a dead
        // device frees its child objects itself, so skipping is correct.
        [[nodiscard]] std::weak_ptr<bool> aliveToken() const { return _aliveToken; }

        // VSM separable gaussian blur — fullscreen draw into the active
        // RenderPassVsmBlur render pass (H: moments → scratch, V: back).
        void executeVsmBlurPass(const VsmBlurPassParams& params, bool horizontal) override;

        // ── Post-processing core (fullscreen draws inside the active pass,
        //    shaders compiled at runtime from engine/shaders/vulkan) ────────
        void executeComposePass(const ComposePassParams& params) override;
        void executeSsaoPass(const SsaoPassParams& params) override;
        void executeDepthAwareBlurPass(const DepthAwareBlurPassParams& params, bool horizontal) override;
        void executeTAAPass(Texture* sourceTexture, Texture* historyTexture, Texture* depthTexture,
            const Matrix4& viewProjectionPrevious, const Matrix4& viewProjectionInverse,
            const std::array<float, 4>& jitters, const std::array<float, 4>& cameraParams,
            bool highQuality, bool historyValid) override;

    private:
        void onFrameStart() override;
        void onFrameEnd() override;

        void initInstance(SDL_Window* window);
        void initDevice();
        void initSwapchain(int width, int height);
        void cleanupSwapchain();
        void createDepthResources();
        void destroyDepthResources();
        void createPerFrameResources();
        void destroyPerFrameResources();
        void createSwapchainSemaphores();
        void destroySwapchainSemaphores();

        // Waits for the device to idle, then rebuilds the swapchain, depth
        // resources, and per-image semaphores at the current _width/_height.
        void recreateSwapchain();

        // Runs queued deferred destroys whose owning frames have provably
        // completed on the GPU (or everything, when force is true — callers
        // must have idled the device first).
        void flushDeferredDestroys(bool force);

        // Record current viewport/scissor/depth-bias state into the live
        // command buffer.  Only valid while a render pass is active.
        void applyViewport();
        void applyScissor();
        void applyDepthBias();

        SDL_Window* _window = nullptr;

        // ── Vulkan core objects ──────────────────────────────────────────
        VkInstance _instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;
        VkSurfaceKHR _surface = VK_NULL_HANDLE;
        VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
        VkDevice _device = VK_NULL_HANDLE;
        VkQueue _graphicsQueue = VK_NULL_HANDLE;
        uint32_t _graphicsQueueFamily = 0;

        // ── Memory allocator ─────────────────────────────────────────────
        VmaAllocator _vmaAllocator = VK_NULL_HANDLE;

        // ── Swapchain ────────────────────────────────────────────────────
        VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
        VkFormat _swapchainFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D _swapchainExtent = {0, 0};
        std::vector<VkImage> _swapchainImages;
        std::vector<VkImageView> _swapchainImageViews;

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

        struct PerFrame {
            VkCommandPool   commandPool    = VK_NULL_HANDLE;
            VkCommandBuffer commandBuffer  = VK_NULL_HANDLE;
            VkSemaphore     imageAvailable = VK_NULL_HANDLE;
            VkFence         inFlightFence  = VK_NULL_HANDLE;
            VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        };
        std::array<PerFrame, kMaxFramesInFlight> _frames{};
        uint32_t _frameIndex = 0;
        uint32_t _swapchainImageIndex = 0;

        // True between a successful acquire in onFrameStart and the submit in
        // onFrameEnd. When acquire fails (even after a swapchain rebuild) the
        // frame is skipped: nothing records, submits, or presents.
        bool _frameActive = false;

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
        // happy (VUID-vkQueueSubmit-pSignalSemaphores-00067).
        std::vector<VkSemaphore> _renderFinishedSemaphores;

        // Layout that the currently-acquired swapchain image is in.  Set to
        // UNDEFINED at frame start; flipped to COLOR_ATTACHMENT_OPTIMAL the
        // first time startRenderPass picks the swapchain target; read by
        // onFrameEnd to decide what source layout to use when transitioning
        // to PRESENT_SRC_KHR.  Without this, frames that never reach
        // startRenderPass (e.g. early UI-only frames during asset loading)
        // declare a wrong source layout and validation fails.
        VkImageLayout _swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // ── Upload resources (for immediate staging transfers) ───────────
        VkCommandPool _uploadCommandPool = VK_NULL_HANDLE;
        VkFence _uploadFence = VK_NULL_HANDLE;

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

        // ── Default resources ────────────────────────────────────────────
        VkSampler _defaultSampler = VK_NULL_HANDLE;
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

        // Packed lighting block; re-uploaded into the ring once per frame (or
        // when setLightingUniforms changes it) and shared by every draw.
        VulkanLightingUBO _lightingUbo{};
        bool _lightingNeedsUpload = true;

        // Once-per-frame throttle for descriptor-pool exhaustion warnings.
        bool _descriptorOverflowWarned = false;

        // Anisotropic-filtering support, resolved at device creation.
        bool _samplerAnisotropyEnabled = false;
        float _maxSamplerAnisotropy = 1.0f;

        // Dies with the device — see aliveToken().
        std::shared_ptr<bool> _aliveToken = std::make_shared<bool>(true);

        // High-res skybox cubemap bound at set 3 binding 6 (white-cube fallback).
        Texture* _skyboxCubeTexture = nullptr;

        // ── VSM blur pass (lazy) ─────────────────────────────────────────
        void ensureVsmBlurResources();
        VkPipeline getVsmBlurPipeline(VkFormat colorFormat, VkFormat depthFormat);
        VkDescriptorSetLayout _vsmBlurSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout _vsmBlurPipelineLayout = VK_NULL_HANDLE;
        VkShaderModule _vsmBlurVertModule = VK_NULL_HANDLE;
        VkShaderModule _vsmBlurFragModule = VK_NULL_HANDLE;
        std::unordered_map<uint64_t, VkPipeline> _vsmBlurPipelines;

        // ── Post-processing framework (vulkanPostProcess.cpp) ────────────
        // Shared layout: bindings 0-3 combined samplers + binding 4 params UBO
        // (sub-allocated from _uniformRing). Pipelines cached per
        // (pass, colorFormat, depthFormat); shaders are runtime-GLSL only —
        // without shaderc the passes no-op (pre-port behavior).
        enum class PostPassKind : uint32_t { Compose = 0, Ssao, DepthBlur, Taa, Count };
        bool ensurePostResources();
        VkShaderModule postFragmentModule(PostPassKind kind);
        VkPipeline getPostPipeline(PostPassKind kind, VkFormat colorFormat, VkFormat depthFormat);
        // Draws a fullscreen triangle with up to 4 textures + a params blob.
        void executePostPass(PostPassKind kind, Texture* const textures[4],
            const void* paramsData, size_t paramsSize);
        void destroyPostResources();

        VkDescriptorSetLayout _postSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout _postPipelineLayout = VK_NULL_HANDLE;
        VkShaderModule _postVertModule = VK_NULL_HANDLE;
        std::array<VkShaderModule, static_cast<size_t>(PostPassKind::Count)> _postFragModules{};
        std::array<bool, static_cast<size_t>(PostPassKind::Count)> _postFragCompileAttempted{};
        VkSampler _postSampler = VK_NULL_HANDLE;   // linear, clamp-to-edge
        std::unordered_map<uint64_t, VkPipeline> _postPipelines;
        bool _postResourcesAttempted = false;
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

        // 1×1 white cubemap: fallback for unbound omni shadow slots (a 2D white
        // view cannot back a samplerCube descriptor).
        VkImage _whiteCubeImage = VK_NULL_HANDLE;
        VmaAllocation _whiteCubeAllocation = VK_NULL_HANDLE;
        VkImageView _whiteCubeImageView = VK_NULL_HANDLE;

        int _width = 0;
        int _height = 0;
    };
}

#endif // VISUTWIN_HAS_VULKAN
