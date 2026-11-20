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
#include "scene/mesh.h"

namespace visutwin::canvas
{
    class BlendState;
    class DepthState;
    class StencilParameters;
    class VulkanGraphicsDevice;
    class VulkanShader;
    class VertexFormat;
    enum class CullMode;

    /**
     * The state a graphics pipeline is built from, which is also its cache key. A VIEW: every
     * pointer and the colour formats are the caller's, read during the call and not held (a
     * draw builds one per draw, so it copies no shared pointer). request() makes the copies its
     * compile job needs (VulkanRenderPipeline::OwnedDesc).
     */
    struct VulkanPipelineDesc
    {
        PrimitiveType primitiveType = PRIMITIVE_TRIANGLES;
        const VertexFormat* vertexFormat = nullptr;
        // The per-instance buffer's format, null for a draw without one.
        const VertexFormat* instanceFormat = nullptr;
        VulkanShader* shader = nullptr;
        const BlendState* blendState = nullptr;
        const DepthState* depthState = nullptr;
        CullMode cullMode{};
        bool stencilEnabled = false;
        // Not const: a stencil state computes its key on first use.
        StencilParameters* stencilFront = nullptr;
        StencilParameters* stencilBack = nullptr;
        std::span<const VkFormat> colorFormats;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        // Raster sample count, which must match the attachments the pass begins rendering
        // with: MSAA targets get their own pipelines.
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        // Selects the depth-pin skybox vertex stage.
        bool isSkybox = false;
    };

    /// The owners of a desc's shader and vertex formats, which a compile job keeps alive.
    struct VulkanPipelineOwners
    {
        std::shared_ptr<VulkanShader> shader;
        std::shared_ptr<VertexFormat> vertexFormat;
        std::shared_ptr<VertexFormat> instanceFormat;
    };

    class VulkanRenderPipeline final : public RenderPipelineBase
    {
    public:
        explicit VulkanRenderPipeline(VulkanGraphicsDevice* device);
        ~VulkanRenderPipeline() override;

        /// The pipelines this cache holds.
        [[nodiscard]] int count() const
        {
            size_t total = 0;
            for (const auto& [hash, entries] : _cache) {
                total += entries.size();
            }
            return static_cast<int>(total);
        }

        /// The pipeline for a state, built on first use. The declared elements of the vertex
        /// and instance formats define its Vulkan vertex bindings.
        VkPipeline get(const VulkanPipelineDesc& desc);

        /// As get(), for a precompile pass: a pipeline the cache does not hold is built on a
        /// compile thread (through the device's pipeline cache, which Vulkan synchronises
        /// internally), and the call returns at once. A later get() of it waits for it.
        /// `owners` own the desc's shader and formats; the mutable states are copied.
        void request(const VulkanPipelineDesc& desc, VulkanPipelineOwners owners);

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
        VkPipeline create(const VulkanPipelineDesc& desc);

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
        uint64_t makeKey(PipelineKey& key, const VulkanPipelineDesc& desc) const;
        std::shared_ptr<CacheEntry> find(uint64_t hash, const PipelineKey& key) const;

        // A desc a compile job owns: the owners of its shader and formats, its own copies of
        // the mutable states (which may change on the requesting thread while the job runs)
        // and of the colour formats. desc() is the view over them.
        struct OwnedDesc
        {
            OwnedDesc(const VulkanPipelineDesc& source, VulkanPipelineOwners sourceOwners);
            [[nodiscard]] VulkanPipelineDesc desc() const;

            VulkanPipelineDesc scalars;   // the pointers and formats are taken from the members below
            VulkanPipelineOwners owners;
            std::shared_ptr<const BlendState> blendState;
            std::shared_ptr<const DepthState> depthState;
            std::shared_ptr<StencilParameters> stencilFront;
            std::shared_ptr<StencilParameters> stencilBack;
            std::array<VkFormat, kMaxKeyColorFormats> colorFormats{};
            size_t colorFormatCount = 0;
        };

        std::unique_ptr<PipelineCompileQueue> _compileQueue;
        std::mutex _readyMutex;
        std::condition_variable _readyCondition;
        int _requestCount = 0;
    };
}

#endif // VISUTWIN_HAS_VULKAN
