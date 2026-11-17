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
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

#include "platform/graphics/pipelineCompileQueue.h"
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
        /// The pipelines this cache holds.
        [[nodiscard]] int count() const
        {
            size_t total = 0;
            for (const auto& [hash, entries] : _cache) {
                total += entries.size();
            }
            return static_cast<int>(total);
        }

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

        /// As get(), for a precompile pass: a pipeline the cache does not hold is built on a
        /// compile thread (through the device's pipeline cache, which Vulkan synchronises
        /// internally), and the call returns at once. A later get() of it waits for it.
        void request(const Primitive& primitive,
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

        /// Pipelines requested and still building.
        [[nodiscard]] int pendingCount() const { return _compileQueue ? _compileQueue->pending() : 0; }
        /// Pipelines requested to build on a compile thread so far.
        [[nodiscard]] int requestCount() const { return _requestCount; }
        void waitIdle()
        {
            if (_compileQueue) {
                _compileQueue->waitIdle();
            }
        }

        [[nodiscard]] VkPipelineLayout pipelineLayout() const { return _pipelineLayout; }
        [[nodiscard]] VkDescriptorSetLayout materialSetLayout() const { return _materialSetLayout; }
        [[nodiscard]] VkDescriptorSetLayout textureSetLayout() const { return _textureSetLayout; }
        [[nodiscard]] VkDescriptorSetLayout lightingSetLayout() const { return _lightingSetLayout; }
        [[nodiscard]] VkDescriptorSetLayout sceneSetLayout() const { return _sceneSetLayout; }
        [[nodiscard]] VkDescriptorSetLayout geometrySetLayout() const { return _geometrySetLayout; }
        [[nodiscard]] VkDescriptorSetLayout clusterSetLayout() const { return _clusterSetLayout; }
        [[nodiscard]] VkDescriptorSetLayout gpuDrivenSetLayout() const { return _gpuDrivenSetLayout; }
        /// EXPERIMENTAL: set kBindlessTableVulkanSet (bindlessTable.h), null without
        /// descriptor indexing; the layout then has seven sets.
        [[nodiscard]] VkDescriptorSetLayout bindlessSetLayout() const { return _bindlessSetLayout; }

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
        VkDescriptorSetLayout _bindlessSetLayout = VK_NULL_HANDLE;

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
            // Written once: by the get() that builds it, or by a compile thread for an entry a
            // precompile requested, which sets `ready` after it.
            std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
            std::atomic<bool> ready{false};
        };
        std::unordered_map<uint64_t, std::vector<std::shared_ptr<CacheEntry>>> _cache;

        // The key words and hash of a pipeline state, and the entry holding it, if any.
        uint64_t makeKey(PipelineKey& key, const Primitive& primitive,
            const std::shared_ptr<VertexFormat>& vertexFormat,
            const std::shared_ptr<VertexFormat>& instanceFormat,
            const std::shared_ptr<VulkanShader>& shader,
            const std::shared_ptr<BlendState>& blendState,
            const std::shared_ptr<DepthState>& depthState,
            CullMode cullMode, bool stencilEnabled,
            const std::shared_ptr<StencilParameters>& stencilFront,
            const std::shared_ptr<StencilParameters>& stencilBack,
            std::span<const VkFormat> colorFormats, VkFormat depthFormat,
            VkSampleCountFlagBits samples, bool isSkybox) const;
        std::shared_ptr<CacheEntry> find(uint64_t hash, const PipelineKey& key) const;

        std::unique_ptr<PipelineCompileQueue> _compileQueue;
        std::mutex _readyMutex;
        std::condition_variable _readyCondition;
        int _requestCount = 0;
    };
}

#endif // VISUTWIN_HAS_VULKAN
