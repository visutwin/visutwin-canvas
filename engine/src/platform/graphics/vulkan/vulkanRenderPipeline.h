// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Vulkan render pipeline — VkPipeline creation, caching, and layout management.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

#include "platform/graphics/renderPipeline.h"

namespace visutwin::canvas
{
    class BlendState;
    class DepthState;
    class StencilParameters;
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

        // The declared elements in both formats define their Vulkan vertex
        // bindings. isSkybox selects the depth-pin skybox vertex stage.
        VkPipeline get(const Primitive& primitive,
            const std::shared_ptr<VertexFormat>& vertexFormat,
            const std::shared_ptr<VertexFormat>& instanceFormat,
            const std::shared_ptr<VulkanShader>& shader,
            const std::shared_ptr<BlendState>& blendState,
            const std::shared_ptr<DepthState>& depthState,
            CullMode cullMode,
            bool stencilEnabled,
            const std::shared_ptr<StencilParameters>& stencilFront,
            const std::shared_ptr<StencilParameters>& stencilBack,
            std::span<const VkFormat> colorFormats,
            VkFormat depthFormat,
            // Raster sample count, which must match the attachments the pass
            // begins rendering with — MSAA targets get their own pipelines.
            VkSampleCountFlagBits samples,
            bool isSkybox = false);

        [[nodiscard]] VkPipelineLayout pipelineLayout() const { return _pipelineLayout; }
        [[nodiscard]] VkDescriptorSetLayout materialSetLayout() const { return _materialSetLayout; }
        [[nodiscard]] VkDescriptorSetLayout textureSetLayout() const { return _textureSetLayout; }
        [[nodiscard]] VkDescriptorSetLayout lightingSetLayout() const { return _lightingSetLayout; }
        [[nodiscard]] VkDescriptorSetLayout sceneSetLayout() const { return _sceneSetLayout; }
        [[nodiscard]] VkDescriptorSetLayout geometrySetLayout() const { return _geometrySetLayout; }
        [[nodiscard]] VkDescriptorSetLayout clusterSetLayout() const { return _clusterSetLayout; }
        [[nodiscard]] VkDescriptorSetLayout gpuDrivenSetLayout() const { return _gpuDrivenSetLayout; }

    private:
        VkPipeline create(const Primitive& primitive,
            const std::shared_ptr<VertexFormat>& vertexFormat,
            const std::shared_ptr<VertexFormat>& instanceFormat,
            const std::shared_ptr<VulkanShader>& shader,
            const std::shared_ptr<BlendState>& blendState,
            const std::shared_ptr<DepthState>& depthState,
            CullMode cullMode,
            bool stencilEnabled,
            const std::shared_ptr<StencilParameters>& stencilFront,
            const std::shared_ptr<StencilParameters>& stencilBack,
            std::span<const VkFormat> colorFormats,
            VkFormat depthFormat,
            VkSampleCountFlagBits samples,
            bool isSkybox);

        void createLayouts();
        void destroy() noexcept;

        VulkanGraphicsDevice* _device;
        VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout _materialSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout _textureSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout _lightingSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout _sceneSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout _geometrySetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout _clusterSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout _gpuDrivenSetLayout = VK_NULL_HANDLE;

        // The full pipeline-state key, compared exactly on a hash hit: a 64-bit hash alone
        // would hand a colliding state the wrong pipeline. Fixed size, so a lookup does not
        // allocate (16 scalar words plus up to kMaxKeyColorFormats colour formats).
        static constexpr size_t kMaxKeyColorFormats = 8;
        struct PipelineKey
        {
            std::array<uint64_t, 16 + kMaxKeyColorFormats> words{};
            uint32_t count = 0;
            bool operator==(const PipelineKey& other) const
            {
                if (count != other.count) return false;
                for (uint32_t i = 0; i < count; ++i) {
                    if (words[i] != other.words[i]) return false;
                }
                return true;
            }
        };
        struct CacheEntry
        {
            PipelineKey key;
            VkPipeline pipeline = VK_NULL_HANDLE;
        };
        std::unordered_map<uint64_t, std::vector<CacheEntry>> _cache;
    };
}

#endif // VISUTWIN_HAS_VULKAN
