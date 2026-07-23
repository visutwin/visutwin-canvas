#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "platform/graphics/instanceCuller.h"

namespace visutwin::canvas
{
    class VulkanGraphicsDevice;

    class VulkanInstanceCullPass final : public InstanceCuller
    {
    public:
        explicit VulkanInstanceCullPass(VulkanGraphicsDevice* device);
        ~VulkanInstanceCullPass() override;

        void reserve(uint32_t maxInstances) override;
        void cull(VertexBuffer* input, const InstanceCullParams& params) override;
        void* compactedNativeBuffer() const override {
            return reinterpret_cast<void*>(_compactedBuffer);
        }
        void* indirectArgsNativeBuffer() const override {
            return reinterpret_cast<void*>(_indirectBuffer);
        }
        uint32_t maxInstances() const override { return _maxInstances; }
        uint32_t visibleCountReadback() const override;

    private:
        bool ensurePipeline();
        VulkanGraphicsDevice* _device = nullptr;
        VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
        VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
        VkPipeline _pipeline = VK_NULL_HANDLE;
        VkBuffer _compactedBuffer = VK_NULL_HANDLE;
        VmaAllocation _compactedAllocation = VK_NULL_HANDLE;
        VkBuffer _indirectBuffer = VK_NULL_HANDLE;
        VmaAllocation _indirectAllocation = VK_NULL_HANDLE;
        void* _indirectMapped = nullptr;
        uint32_t _maxInstances = 0;
    };
}

#endif
