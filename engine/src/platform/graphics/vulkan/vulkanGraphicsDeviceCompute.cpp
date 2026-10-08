// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 27.07.2026
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanGraphicsDevice.h"

#include <algorithm>
#include <optional>
#include <ranges>
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
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"


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
    }



    void VulkanGraphicsDevice::setParticleState(
        const std::shared_ptr<VertexBuffer>& particles,
        const std::shared_ptr<VertexBuffer>& order, const std::shared_ptr<VertexBuffer>& meshVertices,
        const void* params, size_t paramsSize)
    {
        if (!particles || !params || paramsSize == 0 ||
            paramsSize > _pendingParticleParams.size()) {
            _pendingParticleBuffer.reset();
            _pendingParticleOrderBuffer.reset();
            _pendingParticleMeshBuffer.reset();
            _pendingParticleParamsSize = 0;
            return;
        }
        _pendingParticleBuffer = particles;
        _pendingParticleOrderBuffer = order;
        _pendingParticleMeshBuffer = meshVertices;
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

    namespace
    {
        /// What one Compute binds, already resolved to Vulkan objects, in descriptor order.
        struct ComputeBindings
        {
            // One per binding: storage buffers, then textures, then the uniform block.
            std::vector<VkDescriptorType> types;
            std::vector<VkBuffer> storageBuffers;
            std::vector<std::shared_ptr<VulkanVertexBuffer>> storageKeepAlive;
            std::vector<gpu::VulkanTexture*> textures;
            std::vector<VkDescriptorType> textureTypes;
            std::vector<uint8_t> uniformData;
        };

        // One flat descriptor set 0, filled in the order Compute's class comment
        // tabulates: storage buffers (name-sorted) first, then textures
        // (name-sorted), then the loose-uniform block. The KINDS are ordered the
        // same way on Metal, but the INDICES are not — Metal has a namespace per
        // kind, so its textures start at 0 and its uniform block sits right after
        // the buffers. Read the table in compute.h before changing either side.
        // A texture-only compute keeps the bindings it always had.
        std::optional<ComputeBindings> resolveComputeBindings(Compute& compute)
        {
            ComputeBindings bindings;
            for (const auto& buffer : compute.bufferParameters() | std::views::values) {
                auto vkBuffer = std::dynamic_pointer_cast<VulkanVertexBuffer>(buffer);
                if (!vkBuffer || !vkBuffer->buffer()) {
                    spdlog::error("Vulkan compute dispatch '{}' has invalid buffer parameters", compute.name());
                    return std::nullopt;
                }
                bindings.storageBuffers.push_back(vkBuffer->buffer());
                bindings.storageKeepAlive.push_back(std::move(vkBuffer));
                bindings.types.push_back(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            }

            std::vector<std::pair<std::string, Texture*>> parameters(
                compute.textureParameters().begin(),
                compute.textureParameters().end());
            std::ranges::sort(parameters, {}, &decltype(parameters)::value_type::first);
            for (Texture* texture : parameters | std::views::values) {
                // A texture never uploaded has no image yet; upload it now.
                if (texture) {
                    auto* pendingTexture = dynamic_cast<gpu::VulkanTexture*>(texture->impl());
                    if (!pendingTexture || pendingTexture->image() == VK_NULL_HANDLE) {
                        texture->upload();
                    }
                }
                auto* vkTexture = texture ? dynamic_cast<gpu::VulkanTexture*>(texture->impl()) : nullptr;
                if (!vkTexture || vkTexture->image() == VK_NULL_HANDLE) {
                    spdlog::error("Vulkan compute dispatch '{}' has invalid texture parameters", compute.name());
                    return std::nullopt;
                }
                bindings.textures.push_back(vkTexture);
                bindings.textureTypes.push_back(texture->storage()
                    ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                    : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                bindings.types.push_back(bindings.textureTypes.back());
            }

            // Loose scalar uniforms collapse into one UBO bound after the textures.
            bindings.uniformData = compute.uniformData();
            if (!bindings.uniformData.empty()) {
                bindings.types.push_back(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            }

            if (bindings.types.empty()) {
                spdlog::error("Vulkan compute dispatch '{}' has no bound resources", compute.name());
                return std::nullopt;
            }
            return bindings;
        }

        /// A pool sized for exactly one set of these descriptor types.
        VkDescriptorPool createSingleSetPool(VkDevice device, const std::vector<VkDescriptorType>& types)
        {
            std::vector<VkDescriptorPoolSize> poolSizes;
            for (VkDescriptorType type : types) {
                const auto it = std::ranges::find_if(poolSizes,
                    [type](const VkDescriptorPoolSize& size) { return size.type == type; });
                if (it == poolSizes.end()) poolSizes.push_back({type, 1});
                else ++it->descriptorCount;
            }
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
            poolInfo.pPoolSizes = poolSizes.data();
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
                return VK_NULL_HANDLE;
            }
            return pool;
        }

        /// Points every binding of the set at its resource, in ComputeBindings order.
        void writeComputeDescriptors(VkDevice device, VkDescriptorSet descriptorSet,
            const ComputeBindings& bindings, VkBuffer uniformBuffer)
        {
            const uint32_t bufferCount = static_cast<uint32_t>(bindings.storageBuffers.size());
            std::vector<VkDescriptorImageInfo> imageInfos(bindings.textures.size());
            std::vector<VkDescriptorBufferInfo> bufferInfos(bindings.types.size());
            std::vector<VkWriteDescriptorSet> writes(bindings.types.size());
            uint32_t writeCount = 0;
            const auto addWrite = [&](const uint32_t binding, const VkDescriptorType type) -> VkWriteDescriptorSet& {
                auto& write = writes[writeCount++];
                write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = descriptorSet;
                write.dstBinding = binding;
                write.descriptorCount = 1;
                write.descriptorType = type;
                return write;
            };

            for (uint32_t i = 0; i < bufferCount; ++i) {
                bufferInfos[writeCount] = {bindings.storageBuffers[i], 0, VK_WHOLE_SIZE};
                auto& info = bufferInfos[writeCount];
                addWrite(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &info;
            }
            for (uint32_t i = 0; i < bindings.textures.size(); ++i) {
                const bool storage = bindings.textureTypes[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                imageInfos[i].sampler = storage ? VK_NULL_HANDLE : bindings.textures[i]->sampler();
                imageInfos[i].imageView = bindings.textures[i]->imageView();
                imageInfos[i].imageLayout = storage
                    ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                addWrite(bufferCount + i, bindings.textureTypes[i]).pImageInfo = &imageInfos[i];
            }
            if (uniformBuffer != VK_NULL_HANDLE) {
                bufferInfos[writeCount] = {uniformBuffer, 0, bindings.uniformData.size()};
                auto& info = bufferInfos[writeCount];
                addWrite(bufferCount + static_cast<uint32_t>(bindings.textures.size()),
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER).pBufferInfo = &info;
            }
            vkUpdateDescriptorSets(device, writeCount, writes.data(), 0, nullptr);
        }

        /// What the recorded dispatch needs, captured by value: the commands run later,
        /// when the device flushes its upload queue.
        struct ComputeDispatchCommand
        {
            std::vector<gpu::VulkanTexture*> textures;
            std::vector<VkDescriptorType> textureTypes;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            uint32_t groupsX = 1;
            uint32_t groupsY = 1;
            uint32_t groupsZ = 1;
        };

        // Textures go to the layout their binding reads them in, the kernel runs, its
        // writes are made visible to every later stage, and storage images return to
        // SHADER_READ_ONLY for whoever samples them next.
        void recordComputeDispatch(VkCommandBuffer cmd, const ComputeDispatchCommand& command)
        {
            for (uint32_t i = 0; i < command.textures.size(); ++i) {
                command.textures[i]->transitionLayout(cmd,
                    command.textureTypes[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                        ? VK_IMAGE_LAYOUT_GENERAL
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, command.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                command.pipelineLayout, 0, 1, &command.descriptorSet, 0, nullptr);
            vkCmdDispatch(cmd, command.groupsX, command.groupsY, command.groupsZ);

            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            // Every later use: the next kernel's reads AND writes (a reset followed by an
            // atomic count), and a draw's vertex and indirect-argument reads.
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);

            for (uint32_t i = 0; i < command.textures.size(); ++i) {
                if (command.textureTypes[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                    command.textures[i]->transitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            }
        }
    }

    void VulkanGraphicsDevice::computeDispatch(
        const std::vector<Compute*>& computes, const std::string& /*label*/)
    {
        if (_dynamicRenderingActive) {
            spdlog::warn("Vulkan compute dispatch cannot run inside a render pass");
            return;
        }
        for (Compute* compute : computes) {
            if (compute && compute->shader()) {
                enqueueComputeDispatch(*compute);
            }
        }
        flushUploads();
    }

    void VulkanGraphicsDevice::enqueueComputeDispatch(Compute& compute)
    {
        auto shader = std::dynamic_pointer_cast<VulkanShader>(compute.shader());
        if (!shader || shader->computeModule() == VK_NULL_HANDLE) {
            spdlog::error("Vulkan compute dispatch '{}' has no compute module", compute.name());
            return;
        }
        auto bindings = resolveComputeBindings(compute);
        if (!bindings) {
            return;
        }
        const ComputePipelineResources* resources = computePipelineFor(*shader, bindings->types, compute.name());
        if (!resources) {
            return;
        }

        const VkDescriptorPool descriptorPool = createSingleSetPool(_device, bindings->types);
        if (descriptorPool == VK_NULL_HANDLE) {
            return;
        }
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &resources->setLayout;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(_device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
            vkDestroyDescriptorPool(_device, descriptorPool, nullptr);
            return;
        }

        // The uniform block's backing buffer: allocated last, so every failure path above
        // exits without one to release.
        VkBuffer uniformBuffer = VK_NULL_HANDLE;
        VmaAllocation uniformAllocation = VK_NULL_HANDLE;
        if (!bindings->uniformData.empty() &&
            !createComputeUniformBuffer(bindings->uniformData, uniformBuffer, uniformAllocation)) {
            spdlog::error("Vulkan compute dispatch '{}' failed to allocate its uniform block", compute.name());
            vkDestroyDescriptorPool(_device, descriptorPool, nullptr);
            return;
        }

        writeComputeDescriptors(_device, descriptorSet, *bindings, uniformBuffer);

        ComputeDispatchCommand command{bindings->textures, bindings->textureTypes, descriptorSet,
            resources->pipeline, resources->pipelineLayout,
            compute.dispatchX(), compute.dispatchY(), compute.dispatchZ()};
        enqueueUpload(
            [command = std::move(command),
             keepAlive = std::move(bindings->storageKeepAlive)](VkCommandBuffer cmd) {
                (void)keepAlive;
                recordComputeDispatch(cmd, command);
            },
            [device = _device, allocator = _vmaAllocator, descriptorPool,
             uniformBuffer, uniformAllocation] {
                vkDestroyDescriptorPool(device, descriptorPool, nullptr);
                if (uniformBuffer != VK_NULL_HANDLE) {
                    vmaDestroyBuffer(allocator, uniformBuffer, uniformAllocation);
                }
            });
    }

    const VulkanGraphicsDevice::ComputePipelineResources* VulkanGraphicsDevice::computePipelineFor(
        const VulkanShader& shader, const std::vector<VkDescriptorType>& types, const std::string& computeName)
    {
        auto [pipelineIt, inserted] = _computePipelines.try_emplace(shader.id());
        auto& resources = pipelineIt->second;
        if (!inserted) {
            // A pipeline is built for one resource layout; a compute that binds another
            // through the same shader would bind against the wrong set layout.
            if (resources.descriptorTypes != types) {
                spdlog::error("Vulkan compute shader '{}' was rebound with an incompatible resource layout",
                    computeName);
                return nullptr;
            }
            return &resources;
        }

        resources.descriptorTypes = types;
        std::vector<VkDescriptorSetLayoutBinding> bindings(types.size());
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        setInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(_device, &setInfo, nullptr, &resources.setLayout) != VK_SUCCESS) {
            spdlog::error("Failed to create Vulkan compute descriptor layout");
            _computePipelines.erase(pipelineIt);
            return nullptr;
        }

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &resources.setLayout;
        if (vkCreatePipelineLayout(_device, &layoutInfo, nullptr, &resources.pipelineLayout) != VK_SUCCESS) {
            vkDestroyDescriptorSetLayout(_device, resources.setLayout, nullptr);
            _computePipelines.erase(pipelineIt);
            return nullptr;
        }

        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader.computeModule();
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = resources.pipelineLayout;
        notePipelineCreated();
        if (vkCreateComputePipelines(_device, _pipelineCache, 1, &pipelineInfo, nullptr,
                &resources.pipeline) != VK_SUCCESS) {
            vkDestroyPipelineLayout(_device, resources.pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(_device, resources.setLayout, nullptr);
            _computePipelines.erase(pipelineIt);
            return nullptr;
        }
        return &resources;
    }

    bool VulkanGraphicsDevice::createComputeUniformBuffer(const std::vector<uint8_t>& data,
        VkBuffer& buffer, VmaAllocation& allocation)
    {
        // Host-visible and persistently mapped: written once here, read by one dispatch.
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = data.size();
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mappedInfo{};
        if (vmaCreateBuffer(_vmaAllocator, &bufferInfo, &allocationInfo, &buffer, &allocation,
                &mappedInfo) != VK_SUCCESS) {
            return false;
        }
        std::memcpy(mappedInfo.pMappedData, data.data(), data.size());
        vmaFlushAllocation(_vmaAllocator, allocation, 0, data.size());
        return true;
    }

    void VulkanGraphicsDevice::addBackendResourceCounts(LiveResourceCounts& counts) const
    {
        counts.renderPipelines = _renderPipeline ? _renderPipeline->count() : 0;
        counts.computePipelines = static_cast<int>(_computePipelines.size());
    }
}

#endif // VISUTWIN_HAS_VULKAN
