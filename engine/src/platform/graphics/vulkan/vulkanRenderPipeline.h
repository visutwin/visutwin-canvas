// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan render pipeline — VkPipeline creation, caching, and layout management.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <memory>
#include <unordered_map>
#include <vulkan/vulkan.h>

#include "platform/graphics/renderPipeline.h"

namespace visutwin::canvas
{
    class BlendState;
    class DepthState;
    class VulkanGraphicsDevice;
    class VulkanShader;
    class VertexFormat;
    struct Primitive;
    enum class CullMode;

    class VulkanRenderPipeline final : public RenderPipelineBase
    {
    public:
        explicit VulkanRenderPipeline(VulkanGraphicsDevice* device);
        ~VulkanRenderPipeline() override;

        VkPipeline get(const Primitive& primitive,
            const std::shared_ptr<VertexFormat>& vertexFormat,
            const std::shared_ptr<VulkanShader>& shader,
            const std::shared_ptr<BlendState>& blendState,
            const std::shared_ptr<DepthState>& depthState,
            CullMode cullMode,
            VkFormat colorFormat,
            VkFormat depthFormat);

        [[nodiscard]] VkPipelineLayout pipelineLayout() const { return _pipelineLayout; }
        [[nodiscard]] VkDescriptorSetLayout materialSetLayout() const { return _materialSetLayout; }
        [[nodiscard]] VkDescriptorSetLayout textureSetLayout() const { return _textureSetLayout; }

    private:
        VkPipeline create(const Primitive& primitive,
            const std::shared_ptr<VertexFormat>& vertexFormat,
            const std::shared_ptr<VulkanShader>& shader,
            const std::shared_ptr<BlendState>& blendState,
            const std::shared_ptr<DepthState>& depthState,
            CullMode cullMode,
            VkFormat colorFormat,
            VkFormat depthFormat);

        void createLayouts();

        VulkanGraphicsDevice* _device;
        VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout _materialSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout _textureSetLayout = VK_NULL_HANDLE;

        std::unordered_map<uint64_t, VkPipeline> _cache;
    };
}

#endif // VISUTWIN_HAS_VULKAN
