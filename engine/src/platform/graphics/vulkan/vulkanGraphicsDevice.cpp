// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan implementation of the graphics device.
//

#ifdef VISUTWIN_HAS_VULKAN

#define VMA_IMPLEMENTATION
#include "vulkanGraphicsDevice.h"

#include <cstring>
#include <VkBootstrap.h>
#include <SDL3/SDL_vulkan.h>

#include "vulkanIndexBuffer.h"
#include "vulkanRenderPipeline.h"
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

        // Descriptor pool
        std::array<VkDescriptorPoolSize, 2> poolSizes{};
        poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256};
        poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024};

        VkDescriptorPoolCreateInfo dpInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpInfo.maxSets = 512;
        dpInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        dpInfo.pPoolSizes = poolSizes.data();
        dpInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(_device, &dpInfo, nullptr, &_descriptorPool);

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

        spdlog::info("VulkanGraphicsDevice initialized ({}x{})", _width, _height);
    }

    VulkanGraphicsDevice::~VulkanGraphicsDevice()
    {
        if (_device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(_device);

        _renderPipeline.reset();

        if (_defaultSampler != VK_NULL_HANDLE)
            vkDestroySampler(_device, _defaultSampler, nullptr);
        if (_whiteImageView != VK_NULL_HANDLE)
            vkDestroyImageView(_device, _whiteImageView, nullptr);
        if (_whiteImage != VK_NULL_HANDLE)
            vmaDestroyImage(_vmaAllocator, _whiteImage, _whiteAllocation);

        if (_descriptorPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(_device, _descriptorPool, nullptr);

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
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        vkb::PhysicalDeviceSelector selector{vkb::Instance{_instance, _debugMessenger}};
        selector.set_surface(_surface)
                .set_minimum_version(1, 3)
                .set_required_features_13(features13);

        auto physResult = selector.select();
        if (!physResult) {
            spdlog::error("Failed to select Vulkan physical device: {}", physResult.error().message());
            return;
        }
        auto vkbPhysical = physResult.value();
        _physicalDevice = vkbPhysical.physical_device;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(_physicalDevice, &props);
        spdlog::info("Vulkan device: {}", props.deviceName);

        vkb::DeviceBuilder deviceBuilder{vkbPhysical};
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
        vkb::SwapchainBuilder swapBuilder{_physicalDevice, _device, _surface};
        swapBuilder.set_desired_extent(static_cast<uint32_t>(width), static_cast<uint32_t>(height))
                   .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
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
            vkCreateSemaphore(_device, &semInfo, nullptr, &frame.renderFinished);

            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(_device, &fenceInfo, nullptr, &frame.inFlightFence);
        }
    }

    void VulkanGraphicsDevice::destroyPerFrameResources()
    {
        for (auto& frame : _frames) {
            if (frame.inFlightFence != VK_NULL_HANDLE)
                vkDestroyFence(_device, frame.inFlightFence, nullptr);
            if (frame.renderFinished != VK_NULL_HANDLE)
                vkDestroySemaphore(_device, frame.renderFinished, nullptr);
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

        vkResetCommandBuffer(frame.commandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

        // Reset descriptor pool for this frame
        // (simple approach: reset the whole pool each frame)
        vkResetDescriptorPool(_device, _descriptorPool, 0);
    }

    void VulkanGraphicsDevice::onFrameEnd()
    {
        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        // Transition swapchain image → presentable
        vulkanTransitionImageLayout(cmd, _swapchainImages[_swapchainImageIndex],
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        vkEndCommandBuffer(cmd);

        // Submit
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &frame.imageAvailable;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &frame.renderFinished;
        vkQueueSubmit(_graphicsQueue, 1, &submitInfo, frame.inFlightFence);

        // Present
        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &frame.renderFinished;
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

        // Transition swapchain image → color attachment
        vulkanTransitionImageLayout(cmd, _swapchainImages[_swapchainImageIndex],
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        // Transition depth → depth attachment
        VkImageMemoryBarrier depthBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        depthBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthBarrier.srcAccessMask = 0;
        depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.image = _depthImage;
        depthBarrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &depthBarrier);

        // Read clear values from RenderPass
        auto colorOps = renderPass ? renderPass->colorOps() : nullptr;

        VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttachment.imageView = _swapchainImageViews[_swapchainImageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        if (colorOps && colorOps->clear) {
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.clearValue.color = {{
                colorOps->clearValue.r, colorOps->clearValue.g,
                colorOps->clearValue.b, colorOps->clearValue.a}};
        } else {
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        }

        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttachment.imageView = _depthImageView;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};

        auto dsOps = renderPass ? renderPass->depthStencilOps() : nullptr;
        if (dsOps) {
            depthAttachment.loadOp = dsOps->clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depthAttachment.storeOp = dsOps->storeDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAttachment.clearValue.depthStencil = {dsOps->clearDepthValue, 0};
        }

        VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderingInfo.renderArea = {{0, 0}, _swapchainExtent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        renderingInfo.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(cmd, &renderingInfo);

        // Negative viewport height for Y-flip (match Metal/OpenGL convention)
        VkViewport viewport{};
        viewport.x = _vx;
        viewport.y = static_cast<float>(_swapchainExtent.height) - _vy;
        viewport.width = _vw > 0 ? _vw : static_cast<float>(_swapchainExtent.width);
        viewport.height = -(_vh > 0 ? _vh : static_cast<float>(_swapchainExtent.height));
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {_sx, _sy};
        scissor.extent = {
            _sw > 0 ? static_cast<uint32_t>(_sw) : _swapchainExtent.width,
            _sh > 0 ? static_cast<uint32_t>(_sh) : _swapchainExtent.height
        };
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        _dynamicRenderingActive = true;
        _insideRenderPass = true;
        _currentPipeline = VK_NULL_HANDLE;
        _pushConstantsDirty = true;
    }

    void VulkanGraphicsDevice::endRenderPass(RenderPass* renderPass)
    {
        (void)renderPass;
        if (_dynamicRenderingActive) {
            auto& frame = _frames[_frameIndex];
            vkCmdEndRendering(frame.commandBuffer);
            _dynamicRenderingActive = false;
        }
        _insideRenderPass = false;
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

            VkPipeline pipeline = _renderPipeline->get(primitive,
                vf ? vf->format() : nullptr,
                vulkanShader, _blendState, _depthState, _cullMode,
                _swapchainFormat, _depthFormat);

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
        }

        // Push constants (transforms)
        if (_pushConstantsDirty) {
            vkCmdPushConstants(cmd, _renderPipeline->pipelineLayout(),
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &_pushConstants);
            _pushConstantsDirty = false;
        }

        // Bind default texture descriptor set (set 1) if no material textures
        // For now: bind the white fallback texture at binding 0
        {
            VkDescriptorSet texSet;
            VkDescriptorSetAllocateInfo dsAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsAlloc.descriptorPool = _descriptorPool;
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
        memcpy(_pushConstants.viewProjection, viewProjection.c, 64);
        memcpy(_pushConstants.model, model.c, 64);
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
            vulkan_spirv::kForwardBasicFrag, vulkan_spirv::kForwardBasicFragSize);
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
        (void)options;
        // TODO: offscreen render targets
        return nullptr;
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
            cleanupSwapchain();
            initSwapchain(_width, _height);
            createDepthResources();
        }
    }

    std::pair<int, int> VulkanGraphicsDevice::size() const
    {
        return {_width, _height};
    }
}

#endif // VISUTWIN_HAS_VULKAN
