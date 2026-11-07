// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 27.07.2026
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanGraphicsDevice.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <VkBootstrap.h>
#include <SDL3/SDL_vulkan.h>

#include "vulkanIndexBuffer.h"
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
#include "scene/materials/materialMapLanes.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"


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

        // The memo: one slot per layout (a draw asks for the material set and the
        // scene set), the least recently used one replaced.
        LastImageDescriptorSet* memo = nullptr;
        for (auto& last : _lastImageDescriptorSets) {
            if (last.layout == layout && last.serial == _frameSerial) {
                memo = &last;
                break;
            }
        }
        if (memo && memo->count == imageInfos.size() && memo->set != VK_NULL_HANDLE &&
            std::memcmp(memo->infos.data(), imageInfos.data(),
                imageInfos.size() * sizeof(VkDescriptorImageInfo)) == 0) {
            return memo->set;
        }
        if (!memo) {
            memo = &_lastImageDescriptorSets[_lastImageDescriptorSetVictim];
            _lastImageDescriptorSetVictim = (_lastImageDescriptorSetVictim + 1) % _lastImageDescriptorSets.size();
        }
        const auto remember = [&](const VkDescriptorSet set) {
            memo->layout = layout;
            memo->serial = _frameSerial;
            memo->count = static_cast<uint32_t>(imageInfos.size());
            std::memcpy(memo->infos.data(), imageInfos.data(), imageInfos.size() * sizeof(VkDescriptorImageInfo));
            memo->set = set;
            return set;
        };

        ImageDescriptorKey key{};
        key.layout = layout;
        key.count = static_cast<uint32_t>(imageInfos.size());
        for (uint32_t i = 0; i < key.count; ++i) {
            key.samplers[i] = imageInfos[i].sampler;
            key.views[i] = imageInfos[i].imageView;
        }

        auto& cache = _frames[_frameIndex].imageDescriptorCache;
        if (const auto found = cache.find(key); found != cache.end()) {
            return remember(found->second);
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
        return remember(set);
    }

    void VulkanGraphicsDevice::writeUniformRingDescriptors()
    {
        if (!_uniformRing) {
            return;
        }
        // Set 0 carries the material block at binding 0 and the draw's map lanes at binding
        // 1 (materialMapLanes.h), both dynamic uniforms of the ring.
        const std::array<std::tuple<VkDescriptorSet, uint32_t, VkDeviceSize>, 3> sets{{
            {_materialDescriptorSet, 0u, kPerDrawUniformCapacity},
            {_materialDescriptorSet, kBindlessLanesVulkanBinding, sizeof(MaterialMapLanes)},
            {_lightingDescriptorSet, 0u, sizeof(VulkanLightingUBO)},
        }};
        std::array<VkDescriptorBufferInfo, 3> infos{};
        std::array<VkWriteDescriptorSet, 3> writes{};
        uint32_t count = 0;
        for (const auto& [set, binding, range] : sets) {
            if (set == VK_NULL_HANDLE) {
                continue;
            }
            infos[count].buffer = _uniformRing->buffer();
            infos[count].offset = 0;   // base; the per-draw dynamic offset supplies the slot
            infos[count].range = range;
            writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[count].dstSet = set;
            writes[count].dstBinding = binding;
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

    // ---- EXPERIMENTAL bindless table (bindlessTable.h) --------------------------------

    namespace
    {
        class VulkanBindlessTable final : public BindlessTable
        {
        public:
            VulkanGraphicsDevice* device = nullptr;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            VkDescriptorSet set = VK_NULL_HANDLE;
            std::vector<VkSampler> samplers;

            ~VulkanBindlessTable() override
            {
                if (!device) {
                    return;
                }
                device->deferDestroy([vk = device->device(), pool = pool, samplers = samplers] {
                    for (const VkSampler sampler : samplers) {
                        vkDestroySampler(vk, sampler, nullptr);
                    }
                    if (pool != VK_NULL_HANDLE) {
                        vkDestroyDescriptorPool(vk, pool, nullptr);
                    }
                });
            }
        };
    }

    std::shared_ptr<BindlessTable> VulkanGraphicsDevice::createBindlessTable(const BindlessTableDesc& desc)
    {
        if (!_descriptorIndexing || !_renderPipeline || _renderPipeline->bindlessSetLayout() == VK_NULL_HANDLE) {
            spdlog::error("VulkanGraphicsDevice: bindless tables need descriptor indexing");
            return nullptr;
        }
        if (desc.textures.size() > kBindlessTableTextureCapacity || desc.samplers.size() > kBindlessTableSamplerCapacity) {
            spdlog::error("VulkanGraphicsDevice: bindless table of {} textures / {} samplers exceeds {} / {}",
                desc.textures.size(), desc.samplers.size(), kBindlessTableTextureCapacity, kBindlessTableSamplerCapacity);
            return nullptr;
        }
        auto table = std::make_shared<VulkanBindlessTable>();
        table->device = this;

        // Its own pool: the set lives as long as the table, not a frame.
        std::array<VkDescriptorPoolSize, 2> poolSizes{};
        poolSizes[0] = {VK_DESCRIPTOR_TYPE_SAMPLER, kBindlessTableSamplerCapacity};
        poolSizes[1] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            kBindlessTableTextureCapacity + kBindlessTableTextureArrayCapacity + kBindlessTableVolumeCapacity};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(_device, &poolInfo, nullptr, &table->pool) != VK_SUCCESS) {
            spdlog::error("VulkanGraphicsDevice: bindless descriptor pool creation failed");
            return nullptr;
        }
        const VkDescriptorSetLayout layout = _renderPipeline->bindlessSetLayout();
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = table->pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;
        if (vkAllocateDescriptorSets(_device, &allocInfo, &table->set) != VK_SUCCESS) {
            spdlog::error("VulkanGraphicsDevice: bindless descriptor set allocation failed");
            return nullptr;
        }

        std::vector<VkDescriptorImageInfo> samplerInfos;
        for (const TextureSamplerState& state : desc.samplers) {
            const VkSampler sampler = createBindlessSampler(state);
            if (sampler == VK_NULL_HANDLE) {
                return nullptr;
            }
            table->samplers.push_back(sampler);
            samplerInfos.push_back({sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
        }

        // The images, one write per bound entry (an unbound entry stays unwritten, which
        // the partially-bound flag allows); the layout is the one every sampled texture
        // rests in between passes.
        std::vector<VkDescriptorImageInfo> imageInfos;
        std::vector<uint32_t> imageEntries;
        for (size_t i = 0; i < desc.textures.size(); ++i) {
            Texture* texture = desc.textures[i];
            auto* vkTex = texture ? static_cast<gpu::VulkanTexture*>(texture->impl()) : nullptr;
            if (!vkTex || vkTex->imageView() == VK_NULL_HANDLE) {
                continue;
            }
            imageInfos.push_back({VK_NULL_HANDLE, vkTex->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            imageEntries.push_back(static_cast<uint32_t>(i));
        }
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(imageInfos.size() + 1);
        if (!samplerInfos.empty()) {
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = table->set;
            write.dstBinding = 0;
            write.dstArrayElement = 0;
            write.descriptorCount = static_cast<uint32_t>(samplerInfos.size());
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            write.pImageInfo = samplerInfos.data();
            writes.push_back(write);
        }
        for (size_t i = 0; i < imageInfos.size(); ++i) {
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = table->set;
            write.dstBinding = 1;
            write.dstArrayElement = imageEntries[i];
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.pImageInfo = &imageInfos[i];
            writes.push_back(write);
        }
        if (!writes.empty()) {
            vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        spdlog::info("Vulkan bindless table: {} textures and {} samplers in an update-after-bind set "
            "(capacity {} / {})", imageInfos.size(), samplerInfos.size(), kBindlessTableTextureCapacity, kBindlessTableSamplerCapacity);
        return table;
    }

    void VulkanGraphicsDevice::setBindlessTable(BindlessTable* table)
    {
        _bindlessTable = table;
    }

    void VulkanGraphicsDevice::bindBindlessSet(VkCommandBuffer cmd)
    {
        if (auto* table = static_cast<VulkanBindlessTable*>(_bindlessTable)) {
            bindSetIfChanged(cmd, kBindlessTableVulkanSet, table->set);
            return;
        }
        if (const VkDescriptorSet set = _bindlessFrames[_frameIndex].set; set != VK_NULL_HANDLE) {
            // The frame's set, written again before this draw when the registry moved since
            // it was synced: a texture that joins or is recreated (a render target resized in
            // the update) before a draw of the frame is seen by that draw. The set is
            // update-after-bind, so the write is legal while the command buffer records.
            if (BindlessResidency::generation() != _bindlessGeneration ||
                BindlessSamplers::generation() != _bindlessFrames[_frameIndex].samplerGeneration) {
                syncBindlessTable();
            }
            bindSetIfChanged(cmd, kBindlessTableVulkanSet, set);
        }
    }

    VkSampler VulkanGraphicsDevice::createBindlessSampler(const TextureSamplerState& state)
    {
        // The state's filters and wrap modes, as VulkanTexture::createSampler builds a
        // texture's own; the anisotropy clamped to this device's.
        const auto toVkFilter = [](const TextureSamplerState::Filter filter) {
            return filter == TextureSamplerState::Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        };
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = toVkFilter(state.magFilter);
        samplerInfo.minFilter = toVkFilter(state.minFilter);
        samplerInfo.mipmapMode = state.mipFilter == TextureSamplerState::Filter::Nearest
            ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = vulkanMapAddressMode(state.addressU);
        samplerInfo.addressModeV = vulkanMapAddressMode(state.addressV);
        samplerInfo.addressModeW = vulkanMapAddressMode(state.addressW);
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        const float anisotropy = std::min(state.maxAnisotropy, maxAnisotropy());
        samplerInfo.anisotropyEnable = anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy = std::max(anisotropy, 1.0f);
        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(_device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
            spdlog::error("VulkanGraphicsDevice: bindless sampler creation failed");
            return VK_NULL_HANDLE;
        }
        return sampler;
    }

    bool VulkanGraphicsDevice::createBindlessFrameTables()
    {
        const VkDescriptorSetLayout layout = _renderPipeline ? _renderPipeline->bindlessSetLayout() : VK_NULL_HANDLE;
        if (layout == VK_NULL_HANDLE) {
            return false;
        }
        std::array<VkDescriptorPoolSize, 2> poolSizes{};
        poolSizes[0] = {VK_DESCRIPTOR_TYPE_SAMPLER, kBindlessTableSamplerCapacity * kMaxFramesInFlight};
        poolSizes[1] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            (kBindlessTableTextureCapacity + kBindlessTableTextureArrayCapacity + kBindlessTableVolumeCapacity) * kMaxFramesInFlight};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets = kMaxFramesInFlight;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(_device, &poolInfo, nullptr, &_bindlessFramePool) != VK_SUCCESS) {
            return false;
        }
        for (auto& frame : _bindlessFrames) {
            VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocInfo.descriptorPool = _bindlessFramePool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &layout;
            if (vkAllocateDescriptorSets(_device, &allocInfo, &frame.set) != VK_SUCCESS) {
                return false;
            }
            // Nothing written yet, for each kind's array.
            frame.synced[0].assign(kBindlessTableTextureCapacity, UINT64_MAX);
            frame.synced[1].assign(kBindlessTableTextureArrayCapacity, UINT64_MAX);
            frame.synced[2].assign(kBindlessTableVolumeCapacity, UINT64_MAX);
            frame.samplerGeneration = 0;
            frame.samplersWritten = 0;
        }
        return true;
    }

    void VulkanGraphicsDevice::destroyBindlessFrameTables()
    {
        if (_device == VK_NULL_HANDLE) {
            return;
        }
        for (const VkSampler sampler : _bindlessSamplers) {
            vkDestroySampler(_device, sampler, nullptr);
        }
        _bindlessSamplers.clear();
        if (_bindlessFramePool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(_device, _bindlessFramePool, nullptr);
            _bindlessFramePool = VK_NULL_HANDLE;
        }
        for (auto& frame : _bindlessFrames) {
            frame = {};
        }
    }

    void VulkanGraphicsDevice::syncBindlessTable()
    {
        BindlessFrameTable& frame = _bindlessFrames[_frameIndex];
        if (frame.set == VK_NULL_HANDLE) {
            return;
        }
        constexpr std::array<uint32_t, static_cast<size_t>(BindlessKind::Count)> kCapacities{
            kBindlessTableTextureCapacity, kBindlessTableTextureArrayCapacity, kBindlessTableVolumeCapacity};
        constexpr uint32_t kSamplers = kBindlessTableSamplerCapacity;
        static std::vector<VkWriteDescriptorSet> writes;
        static std::vector<VkDescriptorImageInfo> infos;
        writes.clear();
        infos.clear();
        infos.reserve(kCapacities[0] + kCapacities[1] + kCapacities[2] + kSamplers);

        // Samplers: created once per registry index; this slot writes the ones it lacks.
        if (const uint64_t generation = BindlessSamplers::generation(); generation != frame.samplerGeneration) {
            if (_bindlessSamplerStates.size() < kSamplers || _bindlessSamplers.size() < _bindlessSamplerStates.size()) {
                BindlessSamplers::snapshot(_bindlessSamplerStates);
            }
            for (size_t i = _bindlessSamplers.size(); i < _bindlessSamplerStates.size() && i < kSamplers; ++i) {
                const VkSampler sampler = createBindlessSampler(_bindlessSamplerStates[i]);
                if (sampler == VK_NULL_HANDLE) {
                    break;
                }
                _bindlessSamplers.push_back(sampler);
            }
            if (_bindlessSamplerStates.size() > kSamplers) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    spdlog::error("VulkanGraphicsDevice: {} sampler states exceed the bindless table's {}",
                        _bindlessSamplerStates.size(), kSamplers);
                }
            }
            for (size_t i = frame.samplersWritten; i < _bindlessSamplers.size(); ++i) {
                infos.push_back({_bindlessSamplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = frame.set;
                write.dstBinding = 0;
                write.dstArrayElement = static_cast<uint32_t>(i);
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                writes.push_back(write);
            }
            frame.samplersWritten = _bindlessSamplers.size();
            frame.samplerGeneration = generation;
        }

        // Textures of every kind (binding 1 + the kind): the entries this slot has not
        // written at their current version. The snapshots are shared by the slots; they are
        // retaken when the registry moved. An entry with no texture of this device holds the
        // white 2D image for 2D textures; an array or volume entry nothing wrote stays
        // unwritten (partially bound), since no lane names it.
        if (const uint64_t generation = BindlessResidency::generation(); generation != _bindlessGeneration) {
            for (size_t k = 0; k < _bindlessSnapshots.size(); ++k) {
                BindlessResidency::snapshot(_bindlessSnapshots[k], static_cast<BindlessKind>(k));
                if (_bindlessSnapshots[k].size() > kCapacities[k]) {
                    static std::array<bool, static_cast<size_t>(BindlessKind::Count)> warned{};
                    if (!warned[k]) {
                        warned[k] = true;
                        spdlog::error("VulkanGraphicsDevice: {} bindless indices of kind {} exceed the table's {}; the rest read white",
                            _bindlessSnapshots[k].size(), k, kCapacities[k]);
                    }
                }
            }
            _bindlessGeneration = generation;
        }
        for (size_t k = 0; k < kCapacities.size(); ++k) {
            const auto& snapshot = _bindlessSnapshots[k];
            auto& synced = frame.synced[k];
            for (uint32_t i = 0; i < kCapacities[k]; ++i) {
                const uint64_t version = i < snapshot.size() ? snapshot[i].version : 0;
                if (synced[i] == version) {
                    continue;
                }
                VkImageView view = VK_NULL_HANDLE;
                if (i < snapshot.size()) {
                    const Texture* texture = snapshot[i].texture;
                    if (texture && texture->device() == this) {
                        if (auto* vkTex = static_cast<gpu::VulkanTexture*>(texture->impl())) {
                            view = vkTex->imageView();
                        }
                    }
                }
                if (view == VK_NULL_HANDLE && k != 0) {
                    synced[i] = version;
                    continue;
                }
                infos.push_back({VK_NULL_HANDLE, view != VK_NULL_HANDLE ? view : _whiteImageView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = frame.set;
                write.dstBinding = 1u + static_cast<uint32_t>(k);
                write.dstArrayElement = i;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes.push_back(write);
                synced[i] = version;
            }
        }
        // The infos vector is stable now: point each write at its info.
        size_t infoIndex = 0;
        for (auto& write : writes) {
            write.pImageInfo = &infos[infoIndex++];
        }
        if (!writes.empty()) {
            vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

#endif // VISUTWIN_HAS_VULKAN
