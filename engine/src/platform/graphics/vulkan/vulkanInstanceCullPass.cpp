#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanInstanceCullPass.h"

#include <cstring>

#include "vulkanGraphicsDevice.h"
#include "vulkanVertexBuffer.h"
#include "vulkan/vulkan_shader_bundle.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr VkDeviceSize kInstanceSize = 80;
    }

    VulkanInstanceCullPass::VulkanInstanceCullPass(VulkanGraphicsDevice* device)
        : _device(device)
    {
        ensurePipeline();
    }

    VulkanInstanceCullPass::~VulkanInstanceCullPass()
    {
        if (!_device || _device->aliveToken().expired()) return;
        VkDevice vk = _device->device();
        VmaAllocator allocator = _device->vmaAllocator();
        if (_compactedBuffer)
            vmaDestroyBuffer(allocator, _compactedBuffer, _compactedAllocation);
        if (_indirectBuffer)
            vmaDestroyBuffer(allocator, _indirectBuffer, _indirectAllocation);
        if (_pipeline) vkDestroyPipeline(vk, _pipeline, nullptr);
        if (_pipelineLayout) vkDestroyPipelineLayout(vk, _pipelineLayout, nullptr);
        if (_setLayout) vkDestroyDescriptorSetLayout(vk, _setLayout, nullptr);
    }

    bool VulkanInstanceCullPass::ensurePipeline()
    {
        if (_pipeline) return true;
        VkDevice vk = _device->device();
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo setInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        setInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(vk, &setInfo, nullptr, &_setLayout) != VK_SUCCESS)
            return false;
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0,
            sizeof(InstanceCullParams)};
        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &_setLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(vk, &layoutInfo, nullptr, &_pipelineLayout) != VK_SUCCESS)
            return false;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = vulkan_generated::kInstanceCullCompWordCount * sizeof(uint32_t);
        moduleInfo.pCode = vulkan_generated::kInstanceCullComp;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(vk, &moduleInfo, nullptr, &module) != VK_SUCCESS)
            return false;
        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = stage;
        info.layout = _pipelineLayout;
        const VkResult result = vkCreateComputePipelines(
            vk, VK_NULL_HANDLE, 1, &info, nullptr, &_pipeline);
        vkDestroyShaderModule(vk, module, nullptr);
        if (result != VK_SUCCESS) return false;

        return true;
    }

    void VulkanInstanceCullPass::reserve(uint32_t maxInstances)
    {
        if (!ensurePipeline() || maxInstances <= _maxInstances) return;
        VmaAllocator allocator = _device->vmaAllocator();
        if (_compactedBuffer) {
            vmaDestroyBuffer(allocator, _compactedBuffer, _compactedAllocation);
            _compactedBuffer = VK_NULL_HANDLE;
        }
        VkBufferCreateInfo outputInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        outputInfo.size = static_cast<VkDeviceSize>(maxInstances) * kInstanceSize;
        outputInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo gpuAllocation{};
        gpuAllocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateBuffer(allocator, &outputInfo, &gpuAllocation,
                &_compactedBuffer, &_compactedAllocation, nullptr) != VK_SUCCESS) {
            spdlog::error("Vulkan instance culler output allocation failed");
            return;
        }
        if (!_indirectBuffer) {
            VkBufferCreateInfo indirectInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            indirectInfo.size = sizeof(VkDrawIndexedIndirectCommand);
            indirectInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo hostAllocation{};
            hostAllocation.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            hostAllocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocationInfo{};
            if (vmaCreateBuffer(allocator, &indirectInfo, &hostAllocation,
                    &_indirectBuffer, &_indirectAllocation,
                    &allocationInfo) != VK_SUCCESS) {
                spdlog::error("Vulkan instance culler indirect allocation failed");
                return;
            }
            _indirectMapped = allocationInfo.pMappedData;
        }
        _maxInstances = maxInstances;
    }

    void VulkanInstanceCullPass::cull(
        VertexBuffer* input, const InstanceCullParams& params)
    {
        auto* source = dynamic_cast<VulkanVertexBuffer*>(input);
        if (!source || !source->buffer() || params.instanceCount == 0) return;
        reserve(params.instanceCount);
        if (!_compactedBuffer || !_indirectBuffer) return;

        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
        VkDescriptorPoolCreateInfo poolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(_device->device(), &poolInfo, nullptr, &pool) !=
            VK_SUCCESS) return;
        VkDescriptorSetAllocateInfo allocInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &_setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(_device->device(), &allocInfo, &set) != VK_SUCCESS) {
            vkDestroyDescriptorPool(_device->device(), pool, nullptr);
            return;
        }

        std::array<VkDescriptorBufferInfo, 3> infos{{
            {source->buffer(), 0, VK_WHOLE_SIZE},
            {_compactedBuffer, 0, VK_WHOLE_SIZE},
            {_indirectBuffer, 0, sizeof(VkDrawIndexedIndirectCommand)}
        }};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(_device->device(), writes.size(), writes.data(), 0, nullptr);

        VkDrawIndexedIndirectCommand initial{
            params.indexCount, 0, params.indexStart, params.baseVertex,
            params.baseInstance};
        const auto pipeline = _pipeline;
        const auto layout = _pipelineLayout;
        const auto args = _indirectBuffer;
        const auto output = _compactedBuffer;
        _device->enqueueUpload(
            [pipeline, layout, set, args, output, initial, params](VkCommandBuffer cmd) {
                vkCmdUpdateBuffer(cmd, args, 0, sizeof(initial), &initial);
                VkBufferMemoryBarrier2 before{
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                before.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                before.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                before.dstStageMask =
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                before.dstAccessMask =
                    VK_ACCESS_2_SHADER_READ_BIT |
                    VK_ACCESS_2_SHADER_WRITE_BIT;
                before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                before.buffer = args;
                before.size = VK_WHOLE_SIZE;
                VkDependencyInfo dependency{
                    VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.bufferMemoryBarrierCount = 1;
                dependency.pBufferMemoryBarriers = &before;
                vkCmdPipelineBarrier2(cmd, &dependency);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    layout, 0, 1, &set, 0, nullptr);
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(params), &params);
                vkCmdDispatch(cmd, (params.instanceCount + 63) / 64, 1, 1);
                std::array<VkBufferMemoryBarrier2, 2> after{};
                after[0] = {
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                after[0].srcStageMask =
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                after[0].srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                after[0].dstStageMask =
                    VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
                after[0].dstAccessMask =
                    VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
                after[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                after[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                after[0].buffer = output;
                after[0].size = VK_WHOLE_SIZE;
                after[1] = after[0];
                after[1].dstStageMask =
                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
                after[1].dstAccessMask =
                    VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
                after[1].buffer = args;
                dependency.bufferMemoryBarrierCount =
                    static_cast<uint32_t>(after.size());
                dependency.pBufferMemoryBarriers = after.data();
                vkCmdPipelineBarrier2(cmd, &dependency);
            },
            [device = _device->device(), pool] {
                vkDestroyDescriptorPool(device, pool, nullptr);
            });
    }

    uint32_t VulkanInstanceCullPass::visibleCountReadback() const
    {
        if (!_indirectMapped) return 0;
        vmaInvalidateAllocation(_device->vmaAllocator(), _indirectAllocation,
            sizeof(uint32_t), sizeof(uint32_t));
        uint32_t count = 0;
        std::memcpy(&count,
            static_cast<const uint8_t*>(_indirectMapped) + sizeof(uint32_t),
            sizeof(count));
        return count;
    }
}

#endif
