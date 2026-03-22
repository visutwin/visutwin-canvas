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

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"

namespace visutwin::canvas
{
    class VulkanRenderPipeline;

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
            int toneMapping = 0) override;
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
        [[nodiscard]] VkDescriptorPool descriptorPool() const { return _descriptorPool; }

        // Upload command pool for immediate staging transfers
        [[nodiscard]] VkCommandPool uploadCommandPool() const { return _uploadCommandPool; }
        [[nodiscard]] VkFence uploadFence() const { return _uploadFence; }

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
        VmaAllocation _depthAllocation = VK_NULL_HANDLE;
        VkImageView _depthImageView = VK_NULL_HANDLE;
        VkFormat _depthFormat = VK_FORMAT_D32_SFLOAT;

        // ── Per-frame resources (double-buffered) ────────────────────────
        static constexpr uint32_t kMaxFramesInFlight = 2;

        struct PerFrame {
            VkCommandPool   commandPool    = VK_NULL_HANDLE;
            VkCommandBuffer commandBuffer  = VK_NULL_HANDLE;
            VkSemaphore     imageAvailable = VK_NULL_HANDLE;
            VkSemaphore     renderFinished = VK_NULL_HANDLE;
            VkFence         inFlightFence  = VK_NULL_HANDLE;
        };
        std::array<PerFrame, kMaxFramesInFlight> _frames{};
        uint32_t _frameIndex = 0;
        uint32_t _swapchainImageIndex = 0;

        // ── Upload resources (for immediate staging transfers) ───────────
        VkCommandPool _uploadCommandPool = VK_NULL_HANDLE;
        VkFence _uploadFence = VK_NULL_HANDLE;

        // ── Render pipeline ──────────────────────────────────────────────
        std::unique_ptr<VulkanRenderPipeline> _renderPipeline;
        VkPipeline _currentPipeline = VK_NULL_HANDLE;
        bool _dynamicRenderingActive = false;

        // ── Descriptor pool ──────────────────────────────────────────────
        VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;

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

        int _width = 0;
        int _height = 0;
    };
}

#endif // VISUTWIN_HAS_VULKAN
