// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanGraphicsDevice.h"

#include <algorithm>
#include <cstring>
#include <VkBootstrap.h>
#include <SDL3/SDL_vulkan.h>

#include "vulkanIndexBuffer.h"
#include "vulkanInstanceCullPass.h"
#include "vulkanRenderPipeline.h"
#include "vulkanRenderTarget.h"
#include "vulkanShader.h"
#include "vulkanShaderCompiler.h"
#include "vulkanTexture.h"
#include "vulkanUniformRingBuffer.h"
#include "vulkanUtils.h"
#include "vulkanVertexBuffer.h"

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/shaderFeatures.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"

#include "vulkan/vulkan_shader_bundle.h"

namespace visutwin::canvas
{

    bool VulkanGraphicsDevice::initSwapchain(
        const int width, const int height,
        const VkSwapchainKHR oldSwapchain)
    {
        // Use a linear (UNORM) swapchain — the shaders apply manual
        // pow(1/2.2) for display gamma encoding, matching the Metal path
        // which renders into a non-sRGB BGRA8Unorm drawable.  Choosing
        // VK_FORMAT_B8G8R8A8_SRGB instead would make the hardware apply a
        // second sRGB encode on store, doubling the gamma and washing out
        // the rendered scene.
        // Passing both family indices makes vk-bootstrap use concurrent image
        // sharing when graphics and presentation are on different families,
        // avoiding explicit queue-family ownership transfers for each frame.
        vkb::SwapchainBuilder swapBuilder{
            _physicalDevice, _device, _surface,
            _graphicsQueueFamily, _presentQueueFamily};
        swapBuilder.set_desired_extent(static_cast<uint32_t>(width), static_cast<uint32_t>(height))
                   .set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                   .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                   .add_image_usage_flags(
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (oldSwapchain != VK_NULL_HANDLE) {
            swapBuilder.set_old_swapchain(oldSwapchain);
        }

        auto result = swapBuilder.build();
        if (!result) {
            spdlog::error("Failed to create Vulkan swapchain: {}", result.error().message());
            return false;
        }
        auto vkbSwap = result.value();
        auto imagesResult = vkbSwap.get_images();
        auto viewsResult = vkbSwap.get_image_views();
        if (!imagesResult || !viewsResult) {
            if (viewsResult) {
                vkbSwap.destroy_image_views(viewsResult.value());
            }
            vkb::destroy_swapchain(vkbSwap);
            // A successful vkCreateSwapchainKHR retires oldSwapchain even if
            // subsequent setup fails, so the caller must not restore it.
            _swapchain = VK_NULL_HANDLE;
            spdlog::error(
                "Failed to retrieve Vulkan swapchain images or image views");
            return false;
        }

        _swapchain = vkbSwap.swapchain;
        _swapchainFormat = vkbSwap.image_format;
        _swapchainExtent = vkbSwap.extent;
        _swapchainImages = std::move(imagesResult.value());
        _swapchainImageViews = std::move(viewsResult.value());

        // Adopt the actual swapchain extent as the device size.  The requested
        // width/height can be stale or zero before the window is first shown,
        // but the surface clamps the swapchain to its real size. size() (and
        // therefore the renderer's viewport/scissor) must reflect that, or
        // every draw collapses to a 1×1 viewport.
        _width = static_cast<int>(_swapchainExtent.width);
        _height = static_cast<int>(_swapchainExtent.height);
        return true;
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
        _depthFormat = vulkanSupportedDepthFormat(_physicalDevice);
        if (_depthFormat == VK_FORMAT_UNDEFINED) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: no supported swapchain depth format");
        }

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
        if (vmaCreateImage(_vmaAllocator, &imageInfo, &allocInfo,
                &_depthImage, &_depthAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: swapchain depth image allocation failed");
        }

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = _depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = _depthFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(_device, &viewInfo, nullptr, &_depthImageView) !=
            VK_SUCCESS) {
            vmaDestroyImage(_vmaAllocator, _depthImage, _depthAllocation);
            _depthImage = VK_NULL_HANDLE;
            _depthAllocation = VK_NULL_HANDLE;
            throw std::runtime_error(
                "VulkanGraphicsDevice: swapchain depth image view creation failed");
        }
    }

    void VulkanGraphicsDevice::destroyDepthResources()
    {
        if (_depthImageView != VK_NULL_HANDLE)
            vkDestroyImageView(_device, _depthImageView, nullptr);
        if (_depthImage != VK_NULL_HANDLE)
            vmaDestroyImage(_vmaAllocator, _depthImage, _depthAllocation);
        _depthImageView = VK_NULL_HANDLE;
        _depthImage = VK_NULL_HANDLE;
        _depthImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        _depthAllocation = VK_NULL_HANDLE;
    }

    void VulkanGraphicsDevice::createPerFrameResources()
    {
        for (auto& frame : _frames) {
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = _graphicsQueueFamily;
            if (vkCreateCommandPool(
                    _device, &poolInfo, nullptr,
                    &frame.commandPool) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: frame command pool creation failed");
            }

            VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocInfo.commandPool = frame.commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(
                    _device, &allocInfo,
                    &frame.commandBuffer) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: frame command buffer allocation failed");
            }

            VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (vkCreateSemaphore(
                    _device, &semInfo, nullptr,
                    &frame.imageAvailable) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: acquire semaphore creation failed");
            }

            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(
                    _device, &fenceInfo, nullptr,
                    &frame.inFlightFence) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: frame fence creation failed");
            }

            const VkDescriptorPool descriptorPool =
                createFrameDescriptorPool(kInitialDescriptorSets);
            if (descriptorPool == VK_NULL_HANDLE) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: per-frame descriptor pool creation failed");
            }
            frame.descriptorPools.push_back(
                {descriptorPool, kInitialDescriptorSets});
        }

        if (!createSwapchainSemaphores()) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: presentation semaphore creation failed");
        }
    }

    bool VulkanGraphicsDevice::createSwapchainSemaphores()
    {
        // One renderFinished semaphore per swapchain image — recreated
        // alongside the swapchain because the image count can change on
        // resize.
        _renderFinishedSemaphores.resize(_swapchainImages.size(), VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& s : _renderFinishedSemaphores) {
            if (vkCreateSemaphore(
                    _device, &semInfo, nullptr, &s) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
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
            for (const auto& pool : frame.descriptorPools) {
                if (pool.handle != VK_NULL_HANDLE) {
                    vkDestroyDescriptorPool(
                        _device, pool.handle, nullptr);
                }
            }
            if (frame.inFlightFence != VK_NULL_HANDLE)
                vkDestroyFence(_device, frame.inFlightFence, nullptr);
            if (frame.imageAvailable != VK_NULL_HANDLE)
                vkDestroySemaphore(_device, frame.imageAvailable, nullptr);
            if (frame.commandPool != VK_NULL_HANDLE)
                vkDestroyCommandPool(_device, frame.commandPool, nullptr);
            frame = {};
        }
    }

    void VulkanGraphicsDevice::onFrameStart()
    {
        _frameActive = false;
        if (_renderingDisabled) {
            return;
        }
        if (_swapchainRecreationPending) {
            if (!recreateSwapchain() || _swapchainRecreationPending) {
                return;
            }
        }
        auto& frame = _frames[_frameIndex];

        const VkResult waitResult =
            vkWaitForFences(
                _device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
        if (waitResult != VK_SUCCESS) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: frame fence wait failed ({}); "
                "rendering disabled",
                static_cast<int>(waitResult));
            return;
        }
        collectUploads(false);
        flushUploads();

        // The fence wait proves frame (_frameNumber - kMaxFramesInFlight) has
        // completed on the GPU — release resources queued up to that frame.
        flushDeferredDestroys(false);
        collectRetiredSwapchains(false);
        if (_renderingDisabled) {
            return;
        }

        VkResult result = vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX,
            frame.imageAvailable, VK_NULL_HANDLE, &_swapchainImageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Recreate directly — setResolution(_width, _height) would
            // early-return on the unchanged size and never rebuild the
            // swapchain, wedging every subsequent frame.
            if (!recreateSwapchain()) {
                return;
            }
            if (_swapchainRecreationPending) {
                return;
            }
            result = vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX,
                frame.imageAvailable, VK_NULL_HANDLE, &_swapchainImageIndex);
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            // Skip this frame: the fence was deliberately NOT reset, so the
            // next onFrameStart's wait completes immediately. startRenderPass
            // and onFrameEnd check _frameActive and no-op.
            spdlog::warn("VulkanGraphicsDevice: swapchain acquire failed ({}) — skipping frame",
                static_cast<int>(result));
            return;
        }

        _frameActive = true;

        // A fresh swapchain image is always in an undefined layout — vkAcquire
        // doesn't promise any particular contents.  Track this so onFrameEnd
        // can pick the right source layout for the PRESENT transition; and so
        // startRenderPass can use UNDEFINED as the discard source when first
        // attaching the image (which is fine because it gets LOAD_OP_CLEAR /
        // LOAD_OP_DONT_CARE on the colour attachment).
        _swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        const VkResult resetCommandResult =
            vkResetCommandBuffer(frame.commandBuffer, 0);
        if (resetCommandResult != VK_SUCCESS) {
            _frameActive = false;
            recoverFailedFrameSubmission(resetCommandResult);
            return;
        }

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        const VkResult beginResult =
            vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
        if (beginResult != VK_SUCCESS) {
            _frameActive = false;
            recoverFailedFrameSubmission(beginResult);
            return;
        }

        // Advance the uniform ring to this frame's region.  The fence wait
        // above guarantees the GPU has finished reading it, so it is safe to
        // overwrite.  Lighting is re-packed into the fresh region on the next
        // draw (the prior frame's slot offset is now stale).
        if (_uniformRing) {
            _uniformRing->beginFrame(_frameIndex);
        }
        _lightingNeedsUpload = true;

        // Recycle all pools and cached sets owned by this frame slot. The
        // fence wait above guarantees none of them are still in GPU use.
        for (auto& pool : frame.descriptorPools) {
            if (pool.handle == VK_NULL_HANDLE) {
                continue;
            }
            const VkResult resetResult =
                vkResetDescriptorPool(_device, pool.handle, 0);
            if (resetResult == VK_SUCCESS) {
                continue;
            }

            // The frame fence makes destruction safe here. Replace a pool
            // that cannot be reset instead of handing its stale sets back to
            // the allocator.
            vkDestroyDescriptorPool(_device, pool.handle, nullptr);
            pool.handle = createFrameDescriptorPool(pool.maxSets);
            if (pool.handle == VK_NULL_HANDLE &&
                !_descriptorAllocationErrorWarned) {
                _descriptorAllocationErrorWarned = true;
                spdlog::error(
                    "VulkanGraphicsDevice: descriptor pool recycle failed ({})",
                    static_cast<int>(resetResult));
            }
        }
        frame.activeDescriptorPool = 0;
        frame.imageDescriptorCache.clear();
        _descriptorAllocationErrorWarned = false;
        _uniformOverflowWarned = false;
    }

    void VulkanGraphicsDevice::onFrameEnd()
    {
        if (!_frameActive) {
            // Frame was skipped at acquire time — nothing was recorded and the
            // fence was not reset, so there is nothing to submit or present.
            return;
        }
        _frameActive = false;

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

        const VkResult endResult = vkEndCommandBuffer(cmd);
        if (endResult != VK_SUCCESS) {
            recoverFailedFrameSubmission(endResult);
            return;
        }

        VkSemaphore& renderFinished =
            _renderFinishedSemaphores[_swapchainImageIndex];

        // Upload batches use the same queue. Submitting them first gives the
        // frame an implicit queue-order dependency without blocking the CPU.
        flushUploads();

        // Submit. The acquire wait covers the whole command buffer because a
        // no-draw frame can transition directly to PRESENT without reaching
        // COLOR_ATTACHMENT_OUTPUT.
        VkSemaphoreSubmitInfo acquireWait{
            VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        acquireWait.semaphore = frame.imageAvailable;
        acquireWait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo commandInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandInfo.commandBuffer = cmd;
        VkSemaphoreSubmitInfo renderFinishedSignal{
            VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        renderFinishedSignal.semaphore = renderFinished;
        renderFinishedSignal.stageMask =
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &acquireWait;
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &commandInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &renderFinishedSignal;
        // Keep the fence signaled throughout command recording. Reset it only
        // at the last possible moment, when a submission is ready. If reset or
        // submit fails, recovery restores a signaled fence before slot reuse.
        const VkResult resetFenceResult =
            vkResetFences(_device, 1, &frame.inFlightFence);
        if (resetFenceResult != VK_SUCCESS) {
            recoverFailedFrameSubmission(resetFenceResult);
            return;
        }
        VkResult submitResult = VK_SUCCESS;
        if (_submitResultOverride) {
            submitResult = *_submitResultOverride;
            _submitResultOverride.reset();
        } else {
            submitResult = vkQueueSubmit2(
                _graphicsQueue, 1, &submitInfo, frame.inFlightFence);
        }
        if (submitResult != VK_SUCCESS) {
            recoverFailedFrameSubmission(submitResult);
            return;
        }

        // Present
        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &_swapchain;
        presentInfo.pImageIndices = &_swapchainImageIndex;
        const VkResult presentResult =
            vkQueuePresentKHR(_presentQueue, &presentInfo);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
            // Present is the usual place a resize surfaces — rebuild now so the
            // next acquire starts from a valid swapchain.
            (void)recreateSwapchain();
        } else if (presentResult != VK_SUCCESS) {
            spdlog::warn("VulkanGraphicsDevice: vkQueuePresentKHR failed ({})",
                static_cast<int>(presentResult));
        }

        ++_frameNumber;
        _frameIndex = (_frameIndex + 1) % kMaxFramesInFlight;
    }

    void VulkanGraphicsDevice::collectRetiredSwapchains(const bool force)
    {
        if (_retiredSwapchains.empty()) {
            return;
        }

        const auto ready = [this, force](const RetiredSwapchain& retired) {
            return force ||
                retired.frame + kMaxFramesInFlight <= _frameNumber;
        };
        if (!ready(_retiredSwapchains.front())) {
            return;
        }

        // Frame fences cover graphics use of the old image views and depth
        // buffer. Presentation is not covered by those fences, however: its
        // queue may still be consuming renderFinished. Wait only that queue,
        // and only when an old bundle is actually ready to be destroyed.
        if (!force) {
            const VkResult presentWaitResult =
                vkQueueWaitIdle(_presentQueue);
            if (presentWaitResult != VK_SUCCESS) {
                _renderingDisabled = true;
                spdlog::error(
                    "VulkanGraphicsDevice: presentation queue wait while "
                    "retiring a swapchain failed ({}); rendering disabled",
                    static_cast<int>(presentWaitResult));
                return;
            }
        }

        while (!_retiredSwapchains.empty() &&
               ready(_retiredSwapchains.front())) {
            RetiredSwapchain retired =
                std::move(_retiredSwapchains.front());
            _retiredSwapchains.pop_front();

            for (VkSemaphore semaphore :
                 retired.renderFinishedSemaphores) {
                if (semaphore != VK_NULL_HANDLE) {
                    vkDestroySemaphore(_device, semaphore, nullptr);
                }
            }
            for (VkImageView view : retired.imageViews) {
                if (view != VK_NULL_HANDLE) {
                    vkDestroyImageView(_device, view, nullptr);
                }
            }
            if (retired.depthImageView != VK_NULL_HANDLE) {
                vkDestroyImageView(
                    _device, retired.depthImageView, nullptr);
            }
            if (retired.depthImage != VK_NULL_HANDLE) {
                vmaDestroyImage(
                    _vmaAllocator, retired.depthImage,
                    retired.depthAllocation);
            }
            if (retired.swapchain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(
                    _device, retired.swapchain, nullptr);
            }
        }
    }

    void VulkanGraphicsDevice::setResolution(int width, int height)
    {
        if (width == _width && height == _height &&
            !_swapchainRecreationPending) {
            return;
        }
        _width = width;
        _height = height;
        _swapchainRecreationPending = true;
        (void)recreateSwapchain();
    }

    void VulkanGraphicsDevice::recoverFailedFrameSubmission(
        const VkResult result)
    {
        spdlog::error(
            "VulkanGraphicsDevice: frame submission failed ({}); "
            "recovering frame synchronization",
            static_cast<int>(result));

        if (result == VK_ERROR_DEVICE_LOST) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: device lost; rendering disabled");
            return;
        }

        auto& frame = _frames[_frameIndex];

        // The failed submission did not consume imageAvailable. Reusing that
        // binary semaphore in vkAcquireNextImageKHR would try to signal an
        // already-signaled semaphore. Consume it with an empty queue submission
        // and use the frame fence so swapchain recreation can wait for it.
        const VkResult resetResult =
            vkResetFences(_device, 1, &frame.inFlightFence);
        if (resetResult != VK_SUCCESS) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: failed to reset recovery fence ({}); "
                "rendering disabled",
                static_cast<int>(resetResult));
            return;
        }
        VkSemaphoreSubmitInfo acquireWait{
            VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        acquireWait.semaphore = frame.imageAvailable;
        acquireWait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 consumeAcquire{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        consumeAcquire.waitSemaphoreInfoCount = 1;
        consumeAcquire.pWaitSemaphoreInfos = &acquireWait;
        const VkResult recoverySubmit = vkQueueSubmit2(
            _graphicsQueue, 1, &consumeAcquire, frame.inFlightFence);
        if (recoverySubmit != VK_SUCCESS) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: synchronization recovery submit failed "
                "({}); rendering disabled",
                static_cast<int>(recoverySubmit));
            return;
        }

        // The failed frame acquired an image but never presented it. Rebuilding
        // releases that image and gives the next frame a clean WSI state.
        if (!recreateSwapchain()) {
            _renderingDisabled = true;
        }
    }

    bool VulkanGraphicsDevice::recreateSwapchain()
    {
        if (_device == VK_NULL_HANDLE) {
            return false;
        }

        int drawableWidth = 0;
        int drawableHeight = 0;
        const bool drawableSizeAvailable =
            _window != nullptr &&
            SDL_GetWindowSizeInPixels(
                _window, &drawableWidth, &drawableHeight);
        if (_width <= 0 || _height <= 0 ||
            (drawableSizeAvailable &&
             (drawableWidth <= 0 || drawableHeight <= 0))) {
            _swapchainRecreationPending = true;
            return true;
        }

        RetiredSwapchain retired{};
        retired.frame = _frameNumber;
        retired.swapchain = _swapchain;
        retired.imageViews = std::move(_swapchainImageViews);
        retired.depthImage = _depthImage;
        retired.depthAllocation = _depthAllocation;
        retired.depthImageView = _depthImageView;
        retired.renderFinishedSemaphores =
            std::move(_renderFinishedSemaphores);
        const VkImageLayout oldDepthLayout = _depthImageLayout;

        _swapchainImages.clear();
        _depthImage = VK_NULL_HANDLE;
        _depthAllocation = VK_NULL_HANDLE;
        _depthImageView = VK_NULL_HANDLE;
        _depthImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (!initSwapchain(
                _width, _height, retired.swapchain)) {
            // vkCreateSwapchainKHR failure leaves oldSwapchain usable. A
            // later setup failure does not, and initSwapchain marks that by
            // clearing _swapchain after destroying the unusable replacement.
            if (_swapchain == retired.swapchain) {
                _swapchainImageViews =
                    std::move(retired.imageViews);
                _depthImage = retired.depthImage;
                _depthAllocation = retired.depthAllocation;
                _depthImageView = retired.depthImageView;
                _depthImageLayout = oldDepthLayout;
                _renderFinishedSemaphores =
                    std::move(retired.renderFinishedSemaphores);
            } else {
                _retiredSwapchains.push_back(std::move(retired));
            }
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: swapchain recreation failed; "
                "rendering disabled");
            return false;
        }

        _retiredSwapchains.push_back(std::move(retired));

        try {
            createDepthResources();
        } catch (const std::exception& error) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: swapchain depth recreation failed: {}; "
                "rendering disabled",
                error.what());
            return false;
        }
        // Image count may differ in the new swapchain — per-image
        // semaphores must match it.
        if (!createSwapchainSemaphores()) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: presentation semaphore recreation "
                "failed; rendering disabled");
            return false;
        }
        _swapchainRecreationPending = false;
        return true;
    }

    std::pair<int, int> VulkanGraphicsDevice::size() const
    {
        return {_width, _height};
    }
}

#endif // VISUTWIN_HAS_VULKAN
