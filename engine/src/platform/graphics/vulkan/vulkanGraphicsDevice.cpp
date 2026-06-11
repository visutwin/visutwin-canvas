// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan implementation of the graphics device.
//

#ifdef VISUTWIN_HAS_VULKAN

#define VMA_IMPLEMENTATION
#include "vulkanGraphicsDevice.h"

#include <algorithm>
#include <cstring>
#include <VkBootstrap.h>
#include <SDL3/SDL_vulkan.h>

#include "vulkanIndexBuffer.h"
#include "vulkanRenderPipeline.h"
#include "vulkanRenderTarget.h"
#include "vulkanShader.h"
#include "vulkanTexture.h"
#include "vulkanUtils.h"
#include "vulkanVertexBuffer.h"

#include "platform/graphics/renderPass.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"

// Embedded SPIR-V for the basic forward shader.
#include "engine/shaders/vulkan/forward_basic_spirv.h"

namespace visutwin::canvas
{
    // ─────────────────────────────────────────────────────────────────────
    // Construction / Destruction
    // ─────────────────────────────────────────────────────────────────────

    VulkanGraphicsDevice::VulkanGraphicsDevice(const GraphicsDeviceOptions& options)
    {
        _window = options.window;

        int w = 0, h = 0;
        SDL_GetWindowSize(_window, &w, &h);
        _width = w;
        _height = h;

        initInstance(_window);
        initDevice();

        // VMA allocator
        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.physicalDevice = _physicalDevice;
        allocatorInfo.device = _device;
        allocatorInfo.instance = _instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        vmaCreateAllocator(&allocatorInfo, &_vmaAllocator);

        initSwapchain(_width, _height);
        createDepthResources();
        createPerFrameResources();

        // Upload command pool + fence (for staging transfers)
        VkCommandPoolCreateInfo uploadPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        uploadPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        uploadPoolInfo.queueFamilyIndex = _graphicsQueueFamily;
        vkCreateCommandPool(_device, &uploadPoolInfo, nullptr, &_uploadCommandPool);

        VkFenceCreateInfo uploadFenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(_device, &uploadFenceInfo, nullptr, &_uploadFence);

        // Render pipeline
        _renderPipeline = std::make_unique<VulkanRenderPipeline>(this);

        // Default sampler
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        vkCreateSampler(_device, &samplerInfo, nullptr, &_defaultSampler);

        // 1×1 white texture (fallback for unbound texture slots)
        {
            VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imgInfo.imageType = VK_IMAGE_TYPE_2D;
            imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imgInfo.extent = {1, 1, 1};
            imgInfo.mipLevels = 1;
            imgInfo.arrayLayers = 1;
            imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

            VmaAllocationCreateInfo aInfo{};
            aInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            vmaCreateImage(_vmaAllocator, &imgInfo, &aInfo, &_whiteImage, &_whiteAllocation, nullptr);

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = _whiteImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(_device, &viewInfo, nullptr, &_whiteImageView);

            // Upload single white pixel
            uint32_t whitePixel = 0xFFFFFFFF;
            VkBuffer stagingBuf;
            VmaAllocation stagingAlloc;
            VkBufferCreateInfo sInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            sInfo.size = 4;
            sInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo saInfo{};
            saInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
            vmaCreateBuffer(_vmaAllocator, &sInfo, &saInfo, &stagingBuf, &stagingAlloc, nullptr);
            void* mapped;
            vmaMapMemory(_vmaAllocator, stagingAlloc, &mapped);
            memcpy(mapped, &whitePixel, 4);
            vmaUnmapMemory(_vmaAllocator, stagingAlloc);

            vulkanImmediateSubmit(this, [&](VkCommandBuffer cmd) {
                vulkanTransitionImageLayout(cmd, _whiteImage,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {1, 1, 1};
                vkCmdCopyBufferToImage(cmd, stagingBuf, _whiteImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                vulkanTransitionImageLayout(cmd, _whiteImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });
            vmaDestroyBuffer(_vmaAllocator, stagingBuf, stagingAlloc);
        }

        // Default material UBO (white baseColor, identity defaults) — bound
        // at set 0 on every draw until a real material binding path lands.
        {
            VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufInfo.size = 64;
            bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo aInfo{};
            aInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            vmaCreateBuffer(_vmaAllocator, &bufInfo, &aInfo,
                &_defaultMaterialUbo, &_defaultMaterialUboAlloc, &allocInfo);

            struct MiniMaterial {
                float baseColor[4]      = {1.0f, 1.0f, 1.0f, 1.0f};
                float emissiveColor[4]  = {0.0f, 0.0f, 0.0f, 0.0f};
                uint32_t flags          = 0;
                uint32_t occludeSpecMode = 0;
                float alphaCutoff       = 0.0f;
                float metallicFactor    = 0.0f;
                float roughnessFactor   = 1.0f;
                float normalScale       = 1.0f;
                float occlusionStrength = 1.0f;
                float occludeSpecIntensity = 0.0f;
            } mini;
            static_assert(sizeof(MiniMaterial) <= 64);
            memcpy(allocInfo.pMappedData, &mini, sizeof(mini));
        }

        spdlog::info("VulkanGraphicsDevice initialized ({}x{})", _width, _height);
    }

    VulkanGraphicsDevice::~VulkanGraphicsDevice()
    {
        if (_device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(_device);

        _renderPipeline.reset();

        if (_defaultMaterialUbo != VK_NULL_HANDLE)
            vmaDestroyBuffer(_vmaAllocator, _defaultMaterialUbo, _defaultMaterialUboAlloc);
        if (_defaultSampler != VK_NULL_HANDLE)
            vkDestroySampler(_device, _defaultSampler, nullptr);
        if (_whiteImageView != VK_NULL_HANDLE)
            vkDestroyImageView(_device, _whiteImageView, nullptr);
        if (_whiteImage != VK_NULL_HANDLE)
            vmaDestroyImage(_vmaAllocator, _whiteImage, _whiteAllocation);

        destroyPerFrameResources();

        if (_uploadFence != VK_NULL_HANDLE)
            vkDestroyFence(_device, _uploadFence, nullptr);
        if (_uploadCommandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(_device, _uploadCommandPool, nullptr);

        destroyDepthResources();
        cleanupSwapchain();

        if (_vmaAllocator != VK_NULL_HANDLE)
            vmaDestroyAllocator(_vmaAllocator);
        if (_device != VK_NULL_HANDLE)
            vkDestroyDevice(_device, nullptr);
        if (_surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(_instance, _surface, nullptr);
        if (_debugMessenger != VK_NULL_HANDLE) {
            auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(_instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (fn) fn(_instance, _debugMessenger, nullptr);
        }
        if (_instance != VK_NULL_HANDLE)
            vkDestroyInstance(_instance, nullptr);

        spdlog::info("VulkanGraphicsDevice destroyed");
    }

    // ─────────────────────────────────────────────────────────────────────
    // Initialization helpers
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::initInstance(SDL_Window* window)
    {
        vkb::InstanceBuilder builder;
        builder.set_app_name("VisuTwin Canvas")
               .set_engine_name("VisuTwin")
               .require_api_version(1, 3, 0)
               .request_validation_layers(true)
               .use_default_debug_messenger();

        auto result = builder.build();
        if (!result) {
            spdlog::error("Failed to create Vulkan instance: {}", result.error().message());
            return;
        }
        auto vkbInstance = result.value();
        _instance = vkbInstance.instance;
        _debugMessenger = vkbInstance.debug_messenger;

        if (!SDL_Vulkan_CreateSurface(window, _instance, nullptr, &_surface)) {
            spdlog::error("Failed to create Vulkan surface");
        }
    }

    void VulkanGraphicsDevice::initDevice()
    {
        // Require Vulkan 1.3 — dynamicRendering and synchronization2 are
        // promoted-to-core there, so we can use the core entry points
        // directly (vkCmdBeginRendering etc.).  MoltenVK 1.3+ supports this
        // on Apple Silicon.
        //
        // The feature struct is `static` because we hand its address to
        // DeviceBuilder::add_pNext() which keeps the pointer alive until the
        // build() call — vkb's set_required_features_13() vets support but
        // doesn't propagate the struct into VkDeviceCreateInfo::pNext, which
        // leaves dynamicRendering disabled at the device level (validation:
        // VUID-vkCmdBeginRendering-dynamicRendering-06446).  add_pNext pins
        // the feature on so the device creation honours it.
        static VkPhysicalDeviceVulkan13Features features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        // vkb::Instance is an aggregate; construct it field-by-field rather
        // than via a 2-arg ctor (which it doesn't have).
        vkb::Instance vkbInst{};
        vkbInst.instance = _instance;
        vkbInst.debug_messenger = _debugMessenger;
        vkb::PhysicalDeviceSelector selector{vkbInst};
        selector.set_surface(_surface)
                .set_minimum_version(1, 3);

        auto physResult = selector.select();
        if (!physResult) {
            spdlog::error("Failed to select Vulkan physical device: {}", physResult.error().message());
            return;
        }
        auto vkbPhysical = physResult.value();
        _physicalDevice = vkbPhysical.physical_device;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(_physicalDevice, &props);
        spdlog::info("Vulkan device: {}, apiVersion={}.{}.{}", props.deviceName,
            VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion));

        vkb::DeviceBuilder deviceBuilder{vkbPhysical};
        deviceBuilder.add_pNext(&features13);
        auto devResult = deviceBuilder.build();
        if (!devResult) {
            spdlog::error("Failed to create Vulkan device: {}", devResult.error().message());
            return;
        }
        auto vkbDevice = devResult.value();
        _device = vkbDevice.device;

        auto qr = vkbDevice.get_queue(vkb::QueueType::graphics);
        if (qr) _graphicsQueue = qr.value();
        auto qi = vkbDevice.get_queue_index(vkb::QueueType::graphics);
        if (qi) _graphicsQueueFamily = qi.value();
    }

    void VulkanGraphicsDevice::initSwapchain(int width, int height)
    {
        // Use a linear (UNORM) swapchain — the shaders apply manual
        // pow(1/2.2) for display gamma encoding, matching the Metal path
        // which renders into a non-sRGB BGRA8Unorm drawable.  Choosing
        // VK_FORMAT_B8G8R8A8_SRGB instead would make the hardware apply a
        // second sRGB encode on store, doubling the gamma and washing out
        // the rendered scene.
        vkb::SwapchainBuilder swapBuilder{_physicalDevice, _device, _surface};
        swapBuilder.set_desired_extent(static_cast<uint32_t>(width), static_cast<uint32_t>(height))
                   .set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                   .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                   .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT);

        auto result = swapBuilder.build();
        if (!result) {
            spdlog::error("Failed to create Vulkan swapchain: {}", result.error().message());
            return;
        }
        auto vkbSwap = result.value();
        _swapchain = vkbSwap.swapchain;
        _swapchainFormat = vkbSwap.image_format;
        _swapchainExtent = vkbSwap.extent;
        _swapchainImages = vkbSwap.get_images().value();
        _swapchainImageViews = vkbSwap.get_image_views().value();
    }

    void VulkanGraphicsDevice::cleanupSwapchain()
    {
        for (auto view : _swapchainImageViews)
            vkDestroyImageView(_device, view, nullptr);
        _swapchainImageViews.clear();
        _swapchainImages.clear();
        if (_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(_device, _swapchain, nullptr);
            _swapchain = VK_NULL_HANDLE;
        }
    }

    void VulkanGraphicsDevice::createDepthResources()
    {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = _depthFormat;
        imageInfo.extent = {_swapchainExtent.width, _swapchainExtent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(_vmaAllocator, &imageInfo, &allocInfo,
            &_depthImage, &_depthAllocation, nullptr);

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = _depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = _depthFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCreateImageView(_device, &viewInfo, nullptr, &_depthImageView);
    }

    void VulkanGraphicsDevice::destroyDepthResources()
    {
        if (_depthImageView != VK_NULL_HANDLE)
            vkDestroyImageView(_device, _depthImageView, nullptr);
        if (_depthImage != VK_NULL_HANDLE)
            vmaDestroyImage(_vmaAllocator, _depthImage, _depthAllocation);
        _depthImageView = VK_NULL_HANDLE;
        _depthImage = VK_NULL_HANDLE;
        _depthAllocation = VK_NULL_HANDLE;
    }

    void VulkanGraphicsDevice::createPerFrameResources()
    {
        for (auto& frame : _frames) {
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = _graphicsQueueFamily;
            vkCreateCommandPool(_device, &poolInfo, nullptr, &frame.commandPool);

            VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocInfo.commandPool = frame.commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            vkAllocateCommandBuffers(_device, &allocInfo, &frame.commandBuffer);

            VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(_device, &semInfo, nullptr, &frame.imageAvailable);

            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(_device, &fenceInfo, nullptr, &frame.inFlightFence);

            std::array<VkDescriptorPoolSize, 2> poolSizes{};
            poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256};
            poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024};

            VkDescriptorPoolCreateInfo dpInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpInfo.maxSets = 512;
            dpInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
            dpInfo.pPoolSizes = poolSizes.data();
            vkCreateDescriptorPool(_device, &dpInfo, nullptr, &frame.descriptorPool);
        }

        createSwapchainSemaphores();
    }

    void VulkanGraphicsDevice::createSwapchainSemaphores()
    {
        // One renderFinished semaphore per swapchain image — recreated
        // alongside the swapchain because the image count can change on
        // resize.
        _renderFinishedSemaphores.resize(_swapchainImages.size(), VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& s : _renderFinishedSemaphores) {
            vkCreateSemaphore(_device, &semInfo, nullptr, &s);
        }
    }

    void VulkanGraphicsDevice::destroySwapchainSemaphores()
    {
        for (auto& s : _renderFinishedSemaphores) {
            if (s != VK_NULL_HANDLE) vkDestroySemaphore(_device, s, nullptr);
        }
        _renderFinishedSemaphores.clear();
    }

    void VulkanGraphicsDevice::destroyPerFrameResources()
    {
        destroySwapchainSemaphores();

        for (auto& frame : _frames) {
            if (frame.descriptorPool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(_device, frame.descriptorPool, nullptr);
            if (frame.inFlightFence != VK_NULL_HANDLE)
                vkDestroyFence(_device, frame.inFlightFence, nullptr);
            if (frame.imageAvailable != VK_NULL_HANDLE)
                vkDestroySemaphore(_device, frame.imageAvailable, nullptr);
            if (frame.commandPool != VK_NULL_HANDLE)
                vkDestroyCommandPool(_device, frame.commandPool, nullptr);
            frame = {};
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // Frame lifecycle
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::onFrameStart()
    {
        auto& frame = _frames[_frameIndex];

        vkWaitForFences(_device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
        vkResetFences(_device, 1, &frame.inFlightFence);

        VkResult result = vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX,
            frame.imageAvailable, VK_NULL_HANDLE, &_swapchainImageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            setResolution(_width, _height);
            return;
        }

        // A fresh swapchain image is always in an undefined layout — vkAcquire
        // doesn't promise any particular contents.  Track this so onFrameEnd
        // can pick the right source layout for the PRESENT transition; and so
        // startRenderPass can use UNDEFINED as the discard source when first
        // attaching the image (which is fine because it gets LOAD_OP_CLEAR /
        // LOAD_OP_DONT_CARE on the colour attachment).
        _swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        vkResetCommandBuffer(frame.commandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

        // Reset this frame's descriptor pool — fence wait above guarantees
        // the previous frame using this pool has completed on the GPU.
        vkResetDescriptorPool(_device, frame.descriptorPool, 0);
    }

    void VulkanGraphicsDevice::onFrameEnd()
    {
        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        // Transition swapchain image → presentable, using whatever layout
        // the image was left in by the last render pass (or UNDEFINED if no
        // pass touched the swapchain this frame, which is common during
        // asset-load frames before anything is drawn).
        vulkanTransitionImageLayout(cmd, _swapchainImages[_swapchainImageIndex],
            _swapchainImageLayout,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        _swapchainImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        vkEndCommandBuffer(cmd);

        VkSemaphore& renderFinished =
            _renderFinishedSemaphores[_swapchainImageIndex];

        // Submit
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &frame.imageAvailable;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished;
        vkQueueSubmit(_graphicsQueue, 1, &submitInfo, frame.inFlightFence);

        // Present
        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &_swapchain;
        presentInfo.pImageIndices = &_swapchainImageIndex;
        vkQueuePresentKHR(_graphicsQueue, &presentInfo);

        _frameIndex = (_frameIndex + 1) % kMaxFramesInFlight;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Render pass (dynamic rendering, Vulkan 1.3)
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::startRenderPass(RenderPass* renderPass)
    {
        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        auto* offscreen = renderPass
            ? dynamic_cast<VulkanRenderTarget*>(renderPass->renderTarget().get())
            : nullptr;

        // ── Resolve attachment views, formats, extents, and clear ops ──
        auto colorOps = renderPass ? renderPass->colorOps() : nullptr;
        auto dsOps = renderPass ? renderPass->depthStencilOps() : nullptr;

        std::vector<VkRenderingAttachmentInfo> colorInfos;
        VkRenderingAttachmentInfo depthInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        bool hasDepth = false;
        VkExtent2D extent{};

        if (offscreen) {
            // Offscreen: transition each attachment from its current layout
            // (typically SHADER_READ_ONLY from a previous pass, or UNDEFINED
            // on first use) into the appropriate attachment-optimal layout.
            extent = offscreen->extent();

            for (const auto& att : offscreen->colorAttachments()) {
                if (!att.texture) continue;
                const VkImageLayout from = att.texture->currentLayout();
                if (from != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                    vulkanTransitionImageLayout(cmd, att.texture->image(),
                        from, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_ASPECT_COLOR_BIT,
                        0, 1,
                        // For cubemap face attachments target only that face.
                        att.texture->arrayLayers() > 1
                            ? static_cast<uint32_t>(offscreen->face()) : 0u,
                        att.texture->arrayLayers() > 1
                            ? 1u : att.texture->arrayLayers());
                    att.texture->setCurrentLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                }

                VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                info.imageView = att.view;
                info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                if (colorOps && colorOps->clear) {
                    info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    info.clearValue.color = {{colorOps->clearValue.r, colorOps->clearValue.g,
                                              colorOps->clearValue.b, colorOps->clearValue.a}};
                } else {
                    info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                }
                colorInfos.push_back(info);
            }

            if (offscreen->hasDepthAttachment()) {
                hasDepth = true;
                const auto& da = offscreen->depthAttachment();
                const VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;

                // Source image + source layout differ between texture-backed
                // and internally-owned depth.
                VkImage depthImg = da.texture ? da.texture->image() : da.internalImage;
                VkImageLayout fromLayout = da.texture
                    ? da.texture->currentLayout()
                    : da.currentLayout;
                if (depthImg != VK_NULL_HANDLE &&
                    fromLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
                    vulkanTransitionImageLayout(cmd, depthImg,
                        fromLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        depthAspect);
                    if (da.texture) {
                        da.texture->setCurrentLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                    } else {
                        // Internal depth — track via the RT itself.
                        const_cast<VulkanDepthAttachment&>(da).currentLayout =
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    }
                }

                depthInfo.imageView = da.view;
                depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depthInfo.loadOp = (dsOps && dsOps->clearDepth)
                    ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                depthInfo.storeOp = (dsOps && dsOps->storeDepth)
                    ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                depthInfo.clearValue.depthStencil = {dsOps ? dsOps->clearDepthValue : 1.0f, 0};
            }
        } else {
            // Swapchain (back-buffer) path.
            extent = _swapchainExtent;

            if (_swapchainImageLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                vulkanTransitionImageLayout(cmd, _swapchainImages[_swapchainImageIndex],
                    _swapchainImageLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                _swapchainImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            // The shared depth image lives entirely inside one frame's render
            // pass; UNDEFINED→DEPTH_ATTACHMENT discards previous contents,
            // which is what we want before LOAD_OP_CLEAR.
            vulkanTransitionImageLayout(cmd, _depthImage,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_ASPECT_DEPTH_BIT);

            VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            info.imageView = _swapchainImageViews[_swapchainImageIndex];
            info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (colorOps && colorOps->clear) {
                info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                info.clearValue.color = {{colorOps->clearValue.r, colorOps->clearValue.g,
                                          colorOps->clearValue.b, colorOps->clearValue.a}};
            } else {
                info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            }
            colorInfos.push_back(info);

            hasDepth = true;
            depthInfo.imageView = _depthImageView;
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthInfo.loadOp = (dsOps && !dsOps->clearDepth)
                ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthInfo.storeOp = (dsOps && dsOps->storeDepth)
                ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthInfo.clearValue.depthStencil = {dsOps ? dsOps->clearDepthValue : 1.0f, 0};
        }

        VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderingInfo.renderArea = {{0, 0}, extent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorInfos.size());
        renderingInfo.pColorAttachments = colorInfos.empty() ? nullptr : colorInfos.data();
        renderingInfo.pDepthAttachment = hasDepth ? &depthInfo : nullptr;

        vkCmdBeginRendering(cmd, &renderingInfo);

        _activeOffscreenTarget = offscreen;
        _activeExtent = extent;
        _dynamicRenderingActive = true;
        _insideRenderPass = true;
        _currentPipeline = VK_NULL_HANDLE;
        _pushConstantsDirty = true;

        // Every pass starts with a full-target viewport/scissor — same
        // contract as the Metal backend, which resets both at encoder
        // creation.  Camera rects / gizmo viewports are applied afterwards
        // through the setViewport/setScissor overrides.
        GraphicsDevice::setViewport(0.0f, 0.0f,
            static_cast<float>(extent.width), static_cast<float>(extent.height));
        GraphicsDevice::setScissor(0, 0,
            static_cast<int>(extent.width), static_cast<int>(extent.height));
        applyViewport();
        applyScissor();
        applyDepthBias();
    }

    void VulkanGraphicsDevice::endRenderPass(RenderPass* renderPass)
    {
        (void)renderPass;
        if (!_dynamicRenderingActive) {
            _insideRenderPass = false;
            return;
        }

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;
        vkCmdEndRendering(cmd);
        _dynamicRenderingActive = false;

        // Offscreen attachments are usually sampled by a later pass — transition
        // each one back to SHADER_READ_ONLY so the descriptor binding that the
        // next pass writes is valid.  Layout tracking on the texture (or on the
        // RT for internal depth) makes future transitions cheap.
        if (_activeOffscreenTarget) {
            for (const auto& att : _activeOffscreenTarget->colorAttachments()) {
                if (!att.texture) continue;
                vulkanTransitionImageLayout(cmd, att.texture->image(),
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_ASPECT_COLOR_BIT,
                    0, 1,
                    att.texture->arrayLayers() > 1
                        ? static_cast<uint32_t>(_activeOffscreenTarget->face()) : 0u,
                    att.texture->arrayLayers() > 1
                        ? 1u : att.texture->arrayLayers());
                att.texture->setCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            if (_activeOffscreenTarget->hasDepthAttachment()) {
                const auto& da = _activeOffscreenTarget->depthAttachment();
                // Only texture-backed depth can be sampled later — it was
                // created with SAMPLED_BIT.  Internal depth lives only inside
                // the render pass and stays in DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                // until the next pass that uses it (which will start with that
                // same layout).  Transitioning internal depth to
                // SHADER_READ_ONLY is illegal because its image lacks
                // SAMPLED_BIT (VUID-VkImageMemoryBarrier-oldLayout-01211).
                if (da.texture && da.texture->image() != VK_NULL_HANDLE) {
                    vulkanTransitionImageLayout(cmd, da.texture->image(),
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_ASPECT_DEPTH_BIT);
                    da.texture->setCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            }
            _activeOffscreenTarget = nullptr;
        }

        _insideRenderPass = false;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Dynamic state (viewport / scissor / depth bias)
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::setViewport(const float x, const float y, const float w, const float h)
    {
        GraphicsDevice::setViewport(x, y, w, h);
        if (_dynamicRenderingActive) {
            applyViewport();
        }
    }

    void VulkanGraphicsDevice::setScissor(const int x, const int y, const int w, const int h)
    {
        GraphicsDevice::setScissor(x, y, w, h);
        if (_dynamicRenderingActive) {
            applyScissor();
        }
    }

    void VulkanGraphicsDevice::setDepthBias(const float depthBias, const float slopeScale, const float clamp)
    {
        _depthBiasConstant = depthBias;
        _depthBiasSlope = slopeScale;
        _depthBiasClamp = clamp;
        if (_dynamicRenderingActive) {
            applyDepthBias();
        }
    }

    void VulkanGraphicsDevice::applyViewport()
    {
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;

        const float w = vw() > 0.0f ? vw() : static_cast<float>(_activeExtent.width);
        const float h = vh() > 0.0f ? vh() : static_cast<float>(_activeExtent.height);

        // The engine uses a top-left-origin viewport (Metal convention).
        // Vulkan's normal viewport maps NDC +Y downwards; placing the origin
        // on the bottom edge of the rect and negating the height flips it so
        // projection matrices written for Metal/GL work unchanged.
        VkViewport viewport{};
        viewport.x = vx();
        viewport.y = vy() + h;
        viewport.width = w;
        viewport.height = -h;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
    }

    void VulkanGraphicsDevice::applyScissor()
    {
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;

        VkRect2D scissor{};
        scissor.offset = {std::max(sx(), 0), std::max(sy(), 0)};
        scissor.extent = {
            sw() > 0 ? static_cast<uint32_t>(sw()) : _activeExtent.width,
            sh() > 0 ? static_cast<uint32_t>(sh()) : _activeExtent.height
        };
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    void VulkanGraphicsDevice::applyDepthBias()
    {
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;
        vkCmdSetDepthBias(cmd, _depthBiasConstant, _depthBiasClamp, _depthBiasSlope);
    }

    // ─────────────────────────────────────────────────────────────────────
    // Core rendering
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::draw(const Primitive& primitive,
        const std::shared_ptr<IndexBuffer>& indexBuffer,
        int numInstances, int indirectSlot, bool first, bool last)
    {
        (void)indirectSlot;
        if (!_shader || !_dynamicRenderingActive) return;

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        auto vulkanShader = std::dynamic_pointer_cast<VulkanShader>(_shader);
        if (!vulkanShader || vulkanShader->vertexModule() == VK_NULL_HANDLE) return;

        if (first) {
            auto vf = !_vertexBuffers.empty() ? _vertexBuffers[0] : nullptr;

            // Hardware instancing: the renderer binds the per-instance buffer
            // at engine slot 5 with an isInstancing() format (same contract
            // as the Metal backend).  Scan the upper slots for it.
            const VulkanVertexBuffer* instancingVB = nullptr;
            for (size_t i = 1; i < _vertexBuffers.size(); ++i) {
                if (_vertexBuffers[i] && _vertexBuffers[i]->format() &&
                    _vertexBuffers[i]->format()->isInstancing()) {
                    instancingVB = static_cast<VulkanVertexBuffer*>(_vertexBuffers[i].get());
                    break;
                }
            }
            const uint32_t instanceStride = instancingVB && instancingVB->format()
                ? static_cast<uint32_t>(instancingVB->format()->size()) : 0u;

            // Resolve attachment formats for pipeline creation.  The pipeline
            // is keyed on these — a mismatch with the actual VkRenderingInfo
            // attachments at draw-time is rejected by validation as
            // VUID-vkCmdDrawIndexed-dynamicRenderingUnusedAttachments-08910.
            VkFormat colorFmt = _swapchainFormat;
            VkFormat depthFmt = _depthFormat;
            if (_activeOffscreenTarget) {
                const auto& colors = _activeOffscreenTarget->colorAttachments();
                colorFmt = colors.empty() ? VK_FORMAT_UNDEFINED : colors[0].format;
                depthFmt = _activeOffscreenTarget->hasDepthAttachment()
                    ? _activeOffscreenTarget->depthAttachment().format
                    : VK_FORMAT_UNDEFINED;
            }

            VkPipeline pipeline = _renderPipeline->get(primitive,
                vf ? vf->format() : nullptr,
                vulkanShader, _blendState, _depthState, _cullMode,
                colorFmt, depthFmt, instanceStride);

            if (pipeline != _currentPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                _currentPipeline = pipeline;
                _pushConstantsDirty = true;
            }

            // Bind vertex buffer
            if (vf) {
                auto* vb = static_cast<VulkanVertexBuffer*>(vf.get());
                if (vb->buffer() != VK_NULL_HANDLE) {
                    VkBuffer buf = vb->buffer();
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);
                }
            }

            // Bind per-instance buffer at binding 1 (matches the pipeline's
            // VK_VERTEX_INPUT_RATE_INSTANCE binding).
            if (instancingVB && instancingVB->buffer() != VK_NULL_HANDLE) {
                VkBuffer instBuf = instancingVB->buffer();
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 1, 1, &instBuf, &offset);
            }
        }

        // Push constants (transforms)
        if (_pushConstantsDirty) {
            vkCmdPushConstants(cmd, _renderPipeline->pipelineLayout(),
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &_pushConstants);
            _pushConstantsDirty = false;
        }

        // Set 0: default material UBO.  The shader's MaterialData block is
        // statically used, so it MUST be bound — leaving it dangling produces
        // VUID-vkCmdDrawIndexed-None-08600 ("set N is not bound").
        {
            VkDescriptorSet matSet = VK_NULL_HANDLE;
            VkDescriptorSetAllocateInfo dsAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsAlloc.descriptorPool = frame.descriptorPool;
            dsAlloc.descriptorSetCount = 1;
            auto matLayout = _renderPipeline->materialSetLayout();
            dsAlloc.pSetLayouts = &matLayout;
            if (vkAllocateDescriptorSets(_device, &dsAlloc, &matSet) == VK_SUCCESS) {
                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = _defaultMaterialUbo;
                bufInfo.offset = 0;
                bufInfo.range = VK_WHOLE_SIZE;
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = matSet;
                write.dstBinding = 0;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                write.descriptorCount = 1;
                write.pBufferInfo = &bufInfo;
                vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    _renderPipeline->pipelineLayout(), 0, 1, &matSet, 0, nullptr);
            }
        }

        // Bind default texture descriptor set (set 1) if no material textures
        // For now: bind the white fallback texture at binding 0
        {
            VkDescriptorSet texSet;
            VkDescriptorSetAllocateInfo dsAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsAlloc.descriptorPool = frame.descriptorPool;
            dsAlloc.descriptorSetCount = 1;
            auto layout = _renderPipeline->textureSetLayout();
            dsAlloc.pSetLayouts = &layout;

            if (vkAllocateDescriptorSets(_device, &dsAlloc, &texSet) == VK_SUCCESS) {
                // Determine texture to bind
                VkImageView texView = _whiteImageView;
                VkSampler texSampler = _defaultSampler;

                // Check if material has a diffuse map (slot 0 = baseColorMap)
                if (_material) {
                    std::vector<TextureSlot> texSlots;
                    _material->getTextureSlots(texSlots);
                    for (auto& ts : texSlots) {
                        if (ts.slot == 0 && ts.texture != nullptr) {
                            auto* impl = ts.texture->impl();
                            if (impl) {
                                auto* vkTex = static_cast<gpu::VulkanTexture*>(impl);
                                if (vkTex->imageView() != VK_NULL_HANDLE) {
                                    texView = vkTex->imageView();
                                    if (vkTex->sampler() != VK_NULL_HANDLE)
                                        texSampler = vkTex->sampler();
                                }
                            }
                            break;
                        }
                    }
                }

                // Write all 6 texture bindings (use white fallback for unbound slots)
                std::array<VkDescriptorImageInfo, 6> imageInfos{};
                std::array<VkWriteDescriptorSet, 6> writes{};
                for (uint32_t i = 0; i < 6; i++) {
                    imageInfos[i].sampler = _defaultSampler;
                    imageInfos[i].imageView = _whiteImageView;
                    imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[i].dstSet = texSet;
                    writes[i].dstBinding = i;
                    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[i].descriptorCount = 1;
                    writes[i].pImageInfo = &imageInfos[i];
                }
                // Override binding 0 with actual texture
                imageInfos[0].sampler = texSampler;
                imageInfos[0].imageView = texView;

                vkUpdateDescriptorSets(_device, 6, writes.data(), 0, nullptr);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    _renderPipeline->pipelineLayout(), 1, 1, &texSet, 0, nullptr);
            }
        }

        // Draw
        if (indexBuffer) {
            auto* ib = static_cast<VulkanIndexBuffer*>(indexBuffer.get());
            if (ib->buffer() != VK_NULL_HANDLE) {
                VkIndexType idxType = (indexBuffer->format() == INDEXFORMAT_UINT32)
                    ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
                vkCmdBindIndexBuffer(cmd, ib->buffer(), 0, idxType);
                vkCmdDrawIndexed(cmd, primitive.count, numInstances,
                    primitive.base, primitive.baseVertex, 0);
            }
        } else {
            vkCmdDraw(cmd, primitive.count, numInstances, primitive.base, 0);
        }

        recordDrawCall();

        if (last) {
            clearVertexBuffer();
            _currentPipeline = VK_NULL_HANDLE;
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // Uniform setters
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::setTransformUniforms(
        const Matrix4& viewProjection, const Matrix4& model)
    {
        // Matrix4 is alignas(16) and its union always occupies exactly 64
        // bytes of column-major float matrix data — regardless of whether the
        // build uses SSE / NEON / Apple SIMD / scalar storage.  Copy the whole
        // struct as raw bytes; it lands as 16 floats column-major.
        static_assert(sizeof(Matrix4) == 64, "Matrix4 must be 64 bytes");
        memcpy(_pushConstants.viewProjection, &viewProjection, sizeof(Matrix4));
        memcpy(_pushConstants.model, &model, sizeof(Matrix4));
        _pushConstantsDirty = true;
    }

    void VulkanGraphicsDevice::setLightingUniforms(const Color& ambientColor,
        const std::vector<GpuLightData>& lights, const Vector3& cameraPosition,
        bool enableNormalMaps, float exposure, const FogParams& fogParams,
        const ShadowParams& shadowParams, int toneMapping)
    {
        (void)ambientColor; (void)lights; (void)cameraPosition;
        (void)enableNormalMaps; (void)exposure; (void)fogParams;
        (void)shadowParams; (void)toneMapping;
        // TODO: pack into LightingUniforms UBO when full lighting is implemented
    }

    void VulkanGraphicsDevice::setEnvironmentUniforms(
        Texture* envAtlas, float skyboxIntensity, float skyboxMip,
        const Vector3& skyDomeCenter, bool isDome, Texture* skyboxCubeMap)
    {
        (void)envAtlas; (void)skyboxIntensity; (void)skyboxMip;
        (void)skyDomeCenter; (void)isDome; (void)skyboxCubeMap;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Resource creation
    // ─────────────────────────────────────────────────────────────────────

    std::shared_ptr<Shader> VulkanGraphicsDevice::createShader(
        const ShaderDefinition& definition, const std::string& sourceCode)
    {
        (void)sourceCode;
        // Use embedded SPIR-V for the basic forward shader
        return std::make_shared<VulkanShader>(this, definition,
            vulkan_spirv::kForwardBasicVert, vulkan_spirv::kForwardBasicVertSize,
            vulkan_spirv::kForwardBasicFrag, vulkan_spirv::kForwardBasicFragSize,
            vulkan_spirv::kForwardBasicInstancedVert, vulkan_spirv::kForwardBasicInstancedVertSize);
    }

    std::unique_ptr<gpu::HardwareTexture> VulkanGraphicsDevice::createGPUTexture(Texture* texture)
    {
        return std::make_unique<gpu::VulkanTexture>(texture);
    }

    std::shared_ptr<VertexBuffer> VulkanGraphicsDevice::createVertexBuffer(
        const std::shared_ptr<VertexFormat>& format, int numVertices,
        const VertexBufferOptions& options)
    {
        return std::make_shared<VulkanVertexBuffer>(this, format, numVertices, options);
    }

    std::shared_ptr<IndexBuffer> VulkanGraphicsDevice::createIndexBuffer(
        IndexFormat format, int numIndices, const std::vector<uint8_t>& data)
    {
        auto ib = std::make_shared<VulkanIndexBuffer>(this, format, numIndices);
        if (!data.empty()) ib->setData(data);
        return ib;
    }

    std::shared_ptr<RenderTarget> VulkanGraphicsDevice::createRenderTarget(
        const RenderTargetOptions& options)
    {
        // Caller may pass colorBuffer/depthBuffer textures that have not yet
        // had their device assigned; ensure we backfill it before
        // RenderTarget's constructor runs (it asserts on a non-null device).
        RenderTargetOptions opts = options;
        if (!opts.graphicsDevice) {
            opts.graphicsDevice = this;
        }
        return std::make_shared<VulkanRenderTarget>(opts);
    }

    // ─────────────────────────────────────────────────────────────────────
    // Display management
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::setResolution(int width, int height)
    {
        if (width == _width && height == _height) return;
        _width = width;
        _height = height;

        if (_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(_device);
            destroyDepthResources();
            destroySwapchainSemaphores();
            cleanupSwapchain();
            initSwapchain(_width, _height);
            createDepthResources();
            // Image count may differ in the new swapchain — per-image
            // semaphores must match it.
            createSwapchainSemaphores();
        }
    }

    std::pair<int, int> VulkanGraphicsDevice::size() const
    {
        return {_width, _height};
    }
}

#endif // VISUTWIN_HAS_VULKAN
