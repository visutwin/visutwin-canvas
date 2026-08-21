// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanGraphicsDevice.h"

#include <algorithm>
#include <ranges>
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

    void VulkanGraphicsDevice::destroyComputeResources()
    {
        for (auto& [_, resources] : _computePipelines) {
            if (resources.pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(_device, resources.pipeline, nullptr);
            if (resources.pipelineLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(_device, resources.pipelineLayout, nullptr);
            if (resources.setLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(_device, resources.setLayout, nullptr);
        }
        _computePipelines.clear();
        if (_particleSimPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(_device, _particleSimPipeline, nullptr);
        if (_particleSimPipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(_device, _particleSimPipelineLayout, nullptr);
        if (_particleSimSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(_device, _particleSimSetLayout, nullptr);
    }

    bool VulkanGraphicsDevice::ensureParticleSimResources()
    {
        if (_particleSimPipeline != VK_NULL_HANDLE) return true;
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
        }};
        VkDescriptorSetLayoutCreateInfo setInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = bindings.size();
        setInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(_device, &setInfo, nullptr,
                &_particleSimSetLayout) != VK_SUCCESS) return false;
        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &_particleSimSetLayout;
        if (vkCreatePipelineLayout(_device, &layoutInfo, nullptr,
                &_particleSimPipelineLayout) != VK_SUCCESS) return false;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = vulkan_generated::kParticleSimCompWordCount * sizeof(uint32_t);
        moduleInfo.pCode = vulkan_generated::kParticleSimComp;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(_device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
            return false;
        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = _particleSimPipelineLayout;
        const VkResult result = vkCreateComputePipelines(_device, VK_NULL_HANDLE,
            1, &pipelineInfo, nullptr, &_particleSimPipeline);
        vkDestroyShaderModule(_device, module, nullptr);
        return result == VK_SUCCESS;
    }

    void VulkanGraphicsDevice::simulateParticles(
        const std::shared_ptr<VertexBuffer>& particles,
        const GpuParticleSimParams& params)
    {
        auto vkParticles = std::dynamic_pointer_cast<VulkanVertexBuffer>(particles);
        if (!vkParticles || !vkParticles->buffer() ||
            params.timeParams[3] <= 0.0f || !ensureParticleSimResources()) return;

        VkBuffer paramsBuffer = VK_NULL_HANDLE;
        VmaAllocation paramsAllocation = VK_NULL_HANDLE;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = sizeof(params);
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mappedInfo{};
        if (vmaCreateBuffer(_vmaAllocator, &bufferInfo, &allocationInfo,
                &paramsBuffer, &paramsAllocation, &mappedInfo) != VK_SUCCESS) return;
        std::memcpy(mappedInfo.pMappedData, &params, sizeof(params));
        vmaFlushAllocation(_vmaAllocator, paramsAllocation, 0, sizeof(params));

        std::array<VkDescriptorPoolSize, 2> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}
        }};
        VkDescriptorPoolCreateInfo poolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = sizes.size();
        poolInfo.pPoolSizes = sizes.data();
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(_device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            vmaDestroyBuffer(_vmaAllocator, paramsBuffer, paramsAllocation);
            return;
        }
        VkDescriptorSetAllocateInfo setAlloc{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAlloc.descriptorPool = pool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &_particleSimSetLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(_device, &setAlloc, &set) != VK_SUCCESS) {
            vkDestroyDescriptorPool(_device, pool, nullptr);
            vmaDestroyBuffer(_vmaAllocator, paramsBuffer, paramsAllocation);
            return;
        }
        std::array<VkDescriptorBufferInfo, 2> infos{{
            {vkParticles->buffer(), 0, VK_WHOLE_SIZE},
            {paramsBuffer, 0, sizeof(params)}
        }};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = sizes[i].type;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(_device, writes.size(), writes.data(), 0, nullptr);
        const VkPipeline pipeline = _particleSimPipeline;
        const VkPipelineLayout layout = _particleSimPipelineLayout;
        const VkBuffer particleBuffer = vkParticles->buffer();
        const uint32_t groups =
            (static_cast<uint32_t>(params.timeParams[3]) + 255u) / 256u;
        enqueueUpload(
            [pipeline, layout, set, particleBuffer, groups,
             keepAlive = std::move(vkParticles)](VkCommandBuffer cmd) {
                (void)keepAlive;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    layout, 0, 1, &set, 0, nullptr);
                vkCmdDispatch(cmd, groups, 1, 1);
                VkBufferMemoryBarrier2 barrier{
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                barrier.srcStageMask =
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                barrier.dstStageMask =
                    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer = particleBuffer;
                barrier.size = VK_WHOLE_SIZE;
                VkDependencyInfo dependency{
                    VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.bufferMemoryBarrierCount = 1;
                dependency.pBufferMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cmd, &dependency);
            },
            [device = _device, allocator = _vmaAllocator, pool,
             paramsBuffer, paramsAllocation] {
                vkDestroyDescriptorPool(device, pool, nullptr);
                vmaDestroyBuffer(allocator, paramsBuffer, paramsAllocation);
            });
        flushUploads();
    }

    void VulkanGraphicsDevice::setParticleState(
        const std::shared_ptr<VertexBuffer>& particles,
        const void* params, size_t paramsSize)
    {
        if (!particles || !params || paramsSize == 0 ||
            paramsSize > _pendingParticleParams.size()) {
            _pendingParticleBuffer.reset();
            _pendingParticleParamsSize = 0;
            return;
        }
        _pendingParticleBuffer = particles;
        std::memcpy(_pendingParticleParams.data(), params, paramsSize);
        _pendingParticleParamsSize = paramsSize;
    }

    void VulkanGraphicsDevice::setGSplatState(
        const std::shared_ptr<VertexBuffer>& splats,
        const std::shared_ptr<VertexBuffer>& order,
        const std::shared_ptr<VertexBuffer>& sh,
        const void* params, size_t paramsSize)
    {
        if (!splats || !order || !params || paramsSize == 0 ||
            paramsSize > _pendingGSplatParams.size()) {
            _pendingGSplatBuffer.reset();
            _pendingGSplatOrderBuffer.reset();
            _pendingGSplatShBuffer.reset();
            _pendingGSplatParamsSize = 0;
            return;
        }
        _pendingGSplatBuffer = splats;
        _pendingGSplatOrderBuffer = order;
        _pendingGSplatShBuffer = sh;
        std::memcpy(_pendingGSplatParams.data(), params, paramsSize);
        _pendingGSplatParamsSize = paramsSize;
    }

    void VulkanGraphicsDevice::computeDispatch(
        const std::vector<Compute*>& computes, const std::string& label)
    {
        (void)label;
        if (_dynamicRenderingActive) {
            spdlog::warn("Vulkan compute dispatch cannot run inside a render pass");
            return;
        }

        for (Compute* compute : computes) {
            if (!compute || !compute->shader()) continue;
            auto shader = std::dynamic_pointer_cast<VulkanShader>(compute->shader());
            if (!shader || shader->computeModule() == VK_NULL_HANDLE) {
                spdlog::error("Vulkan compute dispatch '{}' has no compute module",
                    compute->name());
                continue;
            }

            // Binding order mirrors the Metal backend and Compute's documented contract:
            // storage buffers (name-sorted) first, then textures (name-sorted), then the
            // loose-uniform block. A texture-only compute keeps the bindings it always had.
            std::vector<VkDescriptorType> types;
            std::vector<VkBuffer> storageBuffers;
            std::vector<std::shared_ptr<VulkanVertexBuffer>> storageKeepAlive;
            bool valid = true;
            for (const auto& buffer : compute->bufferParameters() | std::views::values) {
                auto vkBuffer = std::dynamic_pointer_cast<VulkanVertexBuffer>(buffer);
                if (!vkBuffer || !vkBuffer->buffer()) {
                    valid = false;
                    break;
                }
                storageBuffers.push_back(vkBuffer->buffer());
                storageKeepAlive.push_back(std::move(vkBuffer));
                types.push_back(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            }
            if (!valid) {
                spdlog::error("Vulkan compute dispatch '{}' has invalid buffer parameters",
                    compute->name());
                continue;
            }
            const uint32_t bufferCount = static_cast<uint32_t>(storageBuffers.size());

            std::vector<std::pair<std::string, Texture*>> parameters(
                compute->textureParameters().begin(),
                compute->textureParameters().end());
            std::ranges::sort(parameters, {}, &decltype(parameters)::value_type::first);

            std::vector<gpu::VulkanTexture*> textures;
            std::vector<VkDescriptorType> textureTypes;
            for (const auto& [name, texture] : parameters) {
                (void)name;
                if (texture) {
                    auto* pendingTexture =
                        dynamic_cast<gpu::VulkanTexture*>(texture->impl());
                    if (!pendingTexture ||
                        pendingTexture->image() == VK_NULL_HANDLE) {
                        texture->upload();
                    }
                }
                auto* vkTexture = texture
                    ? dynamic_cast<gpu::VulkanTexture*>(texture->impl()) : nullptr;
                if (!vkTexture || vkTexture->image() == VK_NULL_HANDLE) {
                    valid = false;
                    break;
                }
                textures.push_back(vkTexture);
                textureTypes.push_back(texture->storage()
                    ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                    : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                types.push_back(textureTypes.back());
            }
            if (!valid) {
                spdlog::error("Vulkan compute dispatch '{}' has invalid texture parameters",
                    compute->name());
                continue;
            }

            // Loose scalar uniforms collapse into one UBO bound after the textures. The
            // backing allocation happens further down, once the pipeline is known to exist,
            // so the early-out paths below cannot leak it.
            const std::vector<uint8_t> uniformData = compute->uniformData();
            if (!uniformData.empty()) {
                types.push_back(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            }

            if (types.empty()) {
                spdlog::error("Vulkan compute dispatch '{}' has no bound resources",
                    compute->name());
                continue;
            }

            auto [pipelineIt, inserted] = _computePipelines.try_emplace(shader->id());
            auto& resources = pipelineIt->second;
            if (inserted) {
                resources.descriptorTypes = types;
                std::vector<VkDescriptorSetLayoutBinding> bindings(types.size());
                for (uint32_t i = 0; i < bindings.size(); ++i) {
                    bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
                }
                VkDescriptorSetLayoutCreateInfo setInfo{
                    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
                setInfo.pBindings = bindings.data();
                if (vkCreateDescriptorSetLayout(_device, &setInfo, nullptr,
                        &resources.setLayout) != VK_SUCCESS) {
                    spdlog::error("Failed to create Vulkan compute descriptor layout");
                    _computePipelines.erase(pipelineIt);
                    continue;
                }
                VkPipelineLayoutCreateInfo layoutInfo{
                    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                layoutInfo.setLayoutCount = 1;
                layoutInfo.pSetLayouts = &resources.setLayout;
                if (vkCreatePipelineLayout(_device, &layoutInfo, nullptr,
                        &resources.pipelineLayout) != VK_SUCCESS) {
                    vkDestroyDescriptorSetLayout(_device, resources.setLayout, nullptr);
                    _computePipelines.erase(pipelineIt);
                    continue;
                }
                VkPipelineShaderStageCreateInfo stage{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                stage.module = shader->computeModule();
                stage.pName = "main";
                VkComputePipelineCreateInfo pipelineInfo{
                    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                pipelineInfo.stage = stage;
                pipelineInfo.layout = resources.pipelineLayout;
                if (vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
                        &pipelineInfo, nullptr, &resources.pipeline) != VK_SUCCESS) {
                    vkDestroyPipelineLayout(_device, resources.pipelineLayout, nullptr);
                    vkDestroyDescriptorSetLayout(_device, resources.setLayout, nullptr);
                    _computePipelines.erase(pipelineIt);
                    continue;
                }
            } else if (resources.descriptorTypes != types) {
                spdlog::error(
                    "Vulkan compute shader '{}' was rebound with an incompatible resource layout",
                    compute->name());
                continue;
            }

            std::vector<VkDescriptorPoolSize> poolSizes;
            for (VkDescriptorType type : types) {
                const auto it = std::ranges::find_if(poolSizes,
                    [type](const VkDescriptorPoolSize& size) { return size.type == type; });
                if (it == poolSizes.end()) poolSizes.push_back({type, 1});
                else ++it->descriptorCount;
            }
            VkDescriptorPoolCreateInfo poolInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
            poolInfo.pPoolSizes = poolSizes.data();
            VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(_device, &poolInfo, nullptr,
                    &descriptorPool) != VK_SUCCESS) continue;
            VkDescriptorSetAllocateInfo allocInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocInfo.descriptorPool = descriptorPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &resources.setLayout;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(_device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
                vkDestroyDescriptorPool(_device, descriptorPool, nullptr);
                continue;
            }

            // The uniform block's backing buffer: allocated here so every failure path above
            // exits without one to release.
            VkBuffer uniformBuffer = VK_NULL_HANDLE;
            VmaAllocation uniformAllocation = VK_NULL_HANDLE;
            if (!uniformData.empty()) {
                VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bufferInfo.size = uniformData.size();
                bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                VmaAllocationCreateInfo allocationInfo{};
                allocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo mappedInfo{};
                if (vmaCreateBuffer(_vmaAllocator, &bufferInfo, &allocationInfo,
                        &uniformBuffer, &uniformAllocation, &mappedInfo) != VK_SUCCESS) {
                    spdlog::error("Vulkan compute dispatch '{}' failed to allocate its uniform block",
                        compute->name());
                    vkDestroyDescriptorPool(_device, descriptorPool, nullptr);
                    continue;
                }
                std::memcpy(mappedInfo.pMappedData, uniformData.data(), uniformData.size());
                vmaFlushAllocation(_vmaAllocator, uniformAllocation, 0, uniformData.size());
            }

            std::vector<VkDescriptorImageInfo> imageInfos(textures.size());
            std::vector<VkDescriptorBufferInfo> bufferInfos(types.size());
            std::vector<VkWriteDescriptorSet> writes(types.size());
            uint32_t writeCount = 0;
            for (uint32_t i = 0; i < bufferCount; ++i) {
                bufferInfos[writeCount] = {storageBuffers[i], 0, VK_WHOLE_SIZE};
                auto& write = writes[writeCount];
                write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = descriptorSet;
                write.dstBinding = i;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufferInfos[writeCount];
                ++writeCount;
            }
            for (uint32_t i = 0; i < textures.size(); ++i) {
                const bool storage = textureTypes[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                imageInfos[i].sampler = storage ? VK_NULL_HANDLE : textures[i]->sampler();
                imageInfos[i].imageView = textures[i]->imageView();
                imageInfos[i].imageLayout = storage
                    ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                auto& write = writes[writeCount];
                write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = descriptorSet;
                write.dstBinding = bufferCount + i;
                write.descriptorCount = 1;
                write.descriptorType = textureTypes[i];
                write.pImageInfo = &imageInfos[i];
                ++writeCount;
            }
            if (uniformBuffer != VK_NULL_HANDLE) {
                bufferInfos[writeCount] = {uniformBuffer, 0, uniformData.size()};
                auto& write = writes[writeCount];
                write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = descriptorSet;
                write.dstBinding = bufferCount + static_cast<uint32_t>(textures.size());
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                write.pBufferInfo = &bufferInfos[writeCount];
                ++writeCount;
            }
            vkUpdateDescriptorSets(_device, writeCount, writes.data(), 0, nullptr);

            const VkPipeline pipeline = resources.pipeline;
            const VkPipelineLayout pipelineLayout = resources.pipelineLayout;
            const uint32_t dispatchX = compute->dispatchX();
            const uint32_t dispatchY = compute->dispatchY();
            const uint32_t dispatchZ = compute->dispatchZ();
            enqueueUpload(
                [textures, textureTypes, descriptorSet, pipeline, pipelineLayout,
                 dispatchX, dispatchY, dispatchZ,
                 keepAlive = std::move(storageKeepAlive)](VkCommandBuffer cmd) {
                    (void)keepAlive;
                    for (uint32_t i = 0; i < textures.size(); ++i) {
                        textures[i]->transitionLayout(cmd,
                            textureTypes[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                ? VK_IMAGE_LAYOUT_GENERAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    }
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                        pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
                    vkCmdDispatch(cmd, dispatchX, dispatchY, dispatchZ);
                    VkMemoryBarrier2 barrier{
                        VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                    barrier.srcStageMask =
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                    barrier.dstStageMask =
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                    VkDependencyInfo dependency{
                        VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    dependency.memoryBarrierCount = 1;
                    dependency.pMemoryBarriers = &barrier;
                    vkCmdPipelineBarrier2(cmd, &dependency);
                    for (uint32_t i = 0; i < textures.size(); ++i) {
                        if (textureTypes[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                            textures[i]->transitionLayout(cmd,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        }
                    }
                },
                [device = _device, allocator = _vmaAllocator, descriptorPool,
                 uniformBuffer, uniformAllocation] {
                    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
                    if (uniformBuffer != VK_NULL_HANDLE) {
                        vmaDestroyBuffer(allocator, uniformBuffer, uniformAllocation);
                    }
                });
        }
        flushUploads();
    }
}

#endif // VISUTWIN_HAS_VULKAN
