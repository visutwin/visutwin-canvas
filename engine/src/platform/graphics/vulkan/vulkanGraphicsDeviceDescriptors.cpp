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

    size_t VulkanGraphicsDevice::ImageDescriptorKeyHash::operator()(
        const ImageDescriptorKey& key) const
    {
        size_t hash = std::hash<VkDescriptorSetLayout>{}(key.layout);
        const auto combine = [&hash](const size_t value) {
            hash ^= value + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
        };
        combine(std::hash<uint32_t>{}(key.count));
        for (uint32_t i = 0; i < key.count; ++i) {
            combine(std::hash<VkSampler>{}(key.samplers[i]));
            combine(std::hash<VkImageView>{}(key.views[i]));
        }
        return hash;
    }

    VkDescriptorPool VulkanGraphicsDevice::createFrameDescriptorPool(
        const uint32_t maxSets)
    {
        // Every cached image set has at most seven combined samplers. Post
        // passes additionally consume one ordinary UBO descriptor per set.
        std::array<VkDescriptorPoolSize, 5> poolSizes{};
        poolSizes[0] = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            maxSets * kMaxCachedImageBindings
        };
        poolSizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxSets};
        poolSizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxSets * 2};
        // Set 3 splits its later bindings into separate images plus shared
        // samplers to stay under the per-stage sampler limit.
        poolSizes[3] = {
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            maxSets * kMaxCachedImageBindings
        };
        poolSizes[4] = {VK_DESCRIPTOR_TYPE_SAMPLER, maxSets * 4};

        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = maxSets;
        info.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        info.pPoolSizes = poolSizes.data();

        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(_device, &info, nullptr, &pool) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return pool;
    }

    VkDescriptorSet VulkanGraphicsDevice::allocateFrameDescriptorSet(
        const VkDescriptorSetLayout layout)
    {
        auto& frame = _frames[_frameIndex];
        for (;;) {
            if (frame.activeDescriptorPool >= frame.descriptorPools.size()) {
                constexpr size_t kMaxGrowthSteps = 4;
                const size_t growthStep =
                    std::min(frame.descriptorPools.size(), kMaxGrowthSteps);
                const uint32_t maxSets =
                    kInitialDescriptorSets << static_cast<uint32_t>(growthStep);
                const VkDescriptorPool pool = createFrameDescriptorPool(maxSets);
                if (pool == VK_NULL_HANDLE) {
                    if (!_descriptorAllocationErrorWarned) {
                        _descriptorAllocationErrorWarned = true;
                        spdlog::error(
                            "VulkanGraphicsDevice: descriptor pool growth failed");
                    }
                    return VK_NULL_HANDLE;
                }
                frame.descriptorPools.push_back({pool, maxSets});
            }

            const VkDescriptorPool pool =
                frame.descriptorPools[frame.activeDescriptorPool].handle;
            if (pool == VK_NULL_HANDLE) {
                ++frame.activeDescriptorPool;
                continue;
            }
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkDescriptorSetAllocateInfo info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            info.descriptorPool = pool;
            info.descriptorSetCount = 1;
            info.pSetLayouts = &layout;
            const VkResult result =
                vkAllocateDescriptorSets(_device, &info, &set);
            if (result == VK_SUCCESS) {
                return set;
            }
            if (result == VK_ERROR_OUT_OF_POOL_MEMORY ||
                result == VK_ERROR_FRAGMENTED_POOL) {
                ++frame.activeDescriptorPool;
                continue;
            }
            if (!_descriptorAllocationErrorWarned) {
                _descriptorAllocationErrorWarned = true;
                spdlog::error(
                    "VulkanGraphicsDevice: descriptor allocation failed ({})",
                    static_cast<int>(result));
            }
            return VK_NULL_HANDLE;
        }
    }

    VkDescriptorSet VulkanGraphicsDevice::getOrCreateImageDescriptorSet(
        const VkDescriptorSetLayout layout,
        const std::span<const VkDescriptorImageInfo> imageInfos)
    {
        if (imageInfos.empty() ||
            imageInfos.size() > kMaxCachedImageBindings) {
            return VK_NULL_HANDLE;
        }

        ImageDescriptorKey key{};
        key.layout = layout;
        key.count = static_cast<uint32_t>(imageInfos.size());
        for (uint32_t i = 0; i < key.count; ++i) {
            key.samplers[i] = imageInfos[i].sampler;
            key.views[i] = imageInfos[i].imageView;
        }

        auto& cache = _frames[_frameIndex].imageDescriptorCache;
        if (const auto found = cache.find(key); found != cache.end()) {
            return found->second;
        }

        const VkDescriptorSet set = allocateFrameDescriptorSet(layout);
        if (set == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }

        std::array<VkWriteDescriptorSet, kMaxCachedImageBindings> writes{};
        constexpr auto& materialBindings = kMaterialTextureBindings;
        const bool materialSet =
            layout == _renderPipeline->textureSetLayout() &&
            key.count == materialBindings.size();
        // Set 3's layout is mixed; vulkanSceneDescriptorType is the single
        // source of truth shared with VulkanRenderPipeline's layout creation.
        const bool sceneSet = layout == _renderPipeline->sceneSetLayout();
        for (uint32_t i = 0; i < key.count; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = materialSet ? materialBindings[i] : i;
            if (sceneSet) {
                writes[i].descriptorType = vulkanSceneDescriptorType(i);
            } else if (materialSet) {
                const uint32_t binding = materialBindings[i];
                writes[i].descriptorType =
                    binding == kMaterialExtraSamplerBinding ? VK_DESCRIPTOR_TYPE_SAMPLER
                        : (vulkanMaterialBindingIsSeparateImage(binding)
                               ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            } else {
                writes[i].descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo = &imageInfos[i];
        }
        vkUpdateDescriptorSets(_device, key.count, writes.data(), 0, nullptr);
        cache.emplace(key, set);
        return set;
    }

    void VulkanGraphicsDevice::writeUniformRingDescriptors()
    {
        if (!_uniformRing) {
            return;
        }
        const std::array<std::pair<VkDescriptorSet, VkDeviceSize>, 2> sets{{
            {_materialDescriptorSet, kPerDrawUniformCapacity},
            {_lightingDescriptorSet, sizeof(VulkanLightingUBO)},
        }};
        std::array<VkDescriptorBufferInfo, 2> infos{};
        std::array<VkWriteDescriptorSet, 2> writes{};
        uint32_t count = 0;
        for (const auto& [set, range] : sets) {
            if (set == VK_NULL_HANDLE) {
                continue;
            }
            infos[count].buffer = _uniformRing->buffer();
            infos[count].offset = 0;   // base; the per-draw dynamic offset supplies the slot
            infos[count].range = range;
            writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[count].dstSet = set;
            writes[count].dstBinding = 0;
            writes[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[count].descriptorCount = 1;
            writes[count].pBufferInfo = &infos[count];
            ++count;
        }
        if (count > 0) {
            vkUpdateDescriptorSets(_device, count, writes.data(), 0, nullptr);
        }
    }

    void VulkanGraphicsDevice::growUniformRingIfNeeded()
    {
        if (!_uniformRing || !_uniformRing->wantsGrowth()) {
            return;
        }
        // The old buffer is named by descriptor sets that in-flight command buffers
        // are still using, so nothing may be rebuilt until the queue has drained.
        // This costs a stall, once per size increase — the alternative is a frame
        // whose later draws are simply missing, every frame, for as long as the
        // scene stays this heavy.
        vkDeviceWaitIdle(_device);
        if (_uniformRing->growIfNeeded()) {
            writeUniformRingDescriptors();
        }
    }

    std::optional<uint32_t> VulkanGraphicsDevice::allocateUniform(
        const void* data, const VkDeviceSize size)
    {
        const auto offset = _uniformRing->allocate(data, size);
        if (!offset && !_uniformOverflowReportedThisFrame) {
            _uniformOverflowReportedThisFrame = true;
            spdlog::warn(
                "VulkanGraphicsDevice: uniform ring allocation failed "
                "(requested {}, used {} of {} bytes); the draws past this point are "
                "skipped for this frame and the ring grows before the next one",
                size, _uniformRing->usedBytes(),
                _uniformRing->capacityPerFrame());
        }
        return offset;
    }
}

#endif // VISUTWIN_HAS_VULKAN
