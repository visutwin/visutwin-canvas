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

    void VulkanGraphicsDevice::deferDestroy(std::function<void()> destroyFn)
    {
        if (!destroyFn) {
            return;
        }
        _deferredDestroys.push_back({_frameNumber, std::move(destroyFn)});
    }

    void VulkanGraphicsDevice::enqueueUpload(
        std::function<void(VkCommandBuffer)> record,
        std::function<void()> retire)
    {
        if (!record) {
            return;
        }
        std::lock_guard lock(_uploadMutex);
        _pendingUploads.push_back({std::move(record), std::move(retire)});
    }

    void VulkanGraphicsDevice::flushUploads()
    {
        std::vector<PendingUpload> uploads;
        {
            std::lock_guard lock(_uploadMutex);
            if (_pendingUploads.empty()) {
                return;
            }
            uploads.swap(_pendingUploads);
        }

        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = _uploadCommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(_device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
            spdlog::error("VulkanGraphicsDevice: failed to allocate upload command buffer");
            for (auto& upload : uploads) {
                if (upload.retire) upload.retire();
            }
            return;
        }

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        std::vector<std::function<void()>> retirements;
        retirements.reserve(uploads.size());
        for (auto& upload : uploads) {
            upload.record(commandBuffer);
            if (upload.retire) {
                retirements.push_back(std::move(upload.retire));
            }
        }
        vkEndCommandBuffer(commandBuffer);

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(_device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            vkFreeCommandBuffers(_device, _uploadCommandPool, 1, &commandBuffer);
            for (auto& retire : retirements) retire();
            return;
        }

        VkCommandBufferSubmitInfo commandInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandInfo.commandBuffer = commandBuffer;
        VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &commandInfo;
        if (vkQueueSubmit2(
                _graphicsQueue, 1, &submitInfo, fence) != VK_SUCCESS) {
            vkDestroyFence(_device, fence, nullptr);
            vkFreeCommandBuffers(_device, _uploadCommandPool, 1, &commandBuffer);
            for (auto& retire : retirements) retire();
            return;
        }

        _inFlightUploads.push_back(
            {commandBuffer, fence, std::move(retirements)});
    }

    void VulkanGraphicsDevice::collectUploads(const bool wait)
    {
        // Queue submission order means batches retire from the front.
        while (!_inFlightUploads.empty()) {
            auto& batch = _inFlightUploads.front();
            VkResult status = VK_NOT_READY;
            if (wait) {
                status = vkWaitForFences(
                    _device, 1, &batch.fence, VK_TRUE, UINT64_MAX);
            } else {
                status = vkGetFenceStatus(_device, batch.fence);
            }
            if (status != VK_SUCCESS) {
                break;
            }

            for (auto& retire : batch.retirements) {
                retire();
            }
            vkDestroyFence(_device, batch.fence, nullptr);
            vkFreeCommandBuffers(
                _device, _uploadCommandPool, 1, &batch.commandBuffer);
            _inFlightUploads.pop_front();
        }
    }

    void VulkanGraphicsDevice::flushDeferredDestroys(const bool force)
    {
        while (!_deferredDestroys.empty()) {
            const auto& front = _deferredDestroys.front();
            // A resource queued during frame N may be referenced by frame N's
            // own command buffer and the kMaxFramesInFlight-1 earlier ones;
            // frame N's fence has provably signaled once _frameNumber reaches
            // N + kMaxFramesInFlight (single queue, in-order completion).
            if (!force && front.frame + kMaxFramesInFlight > _frameNumber) {
                break;
            }
            auto fn = std::move(_deferredDestroys.front().fn);
            _deferredDestroys.pop_front();
            fn();
        }
    }
}

#endif // VISUTWIN_HAS_VULKAN
