// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.07.2025
//
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include "Metal/Metal.hpp"
#include "metalGraphicsDevice.h"
#include "metalPipeline.h"
#include "platform/graphics/bindGroupFormat.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/pipelineCompileQueue.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/renderPipeline.h"
#include "platform/graphics/stencilParameters.h"
#include "scene/mesh.h"

namespace visutwin::canvas
{
    /**
     * Cache entry for storing render pipelines
     */
    struct CacheEntry {
        // Render pipeline. Written once: by the lookup that builds it, or by a compile thread
        // for an entry a precompile requested, which sets `ready` after it.
        std::atomic<MTL::RenderPipelineState*> pipeline{nullptr};
        std::atomic<bool> ready{false};

        /** The full array of hashes used to look up the pipeline, used in case of hash collision */
        std::vector<uint32_t> hashes;
    };

    /// The attachment formats a pipeline is built for, read from its render target on the
    /// render thread, so a compile thread never touches the target.
    struct MetalPipelineTargetFormats
    {
        bool backBuffer = true;
        std::array<MTL::PixelFormat, 8> colors{};
        size_t colorCount = 0;
        MTL::PixelFormat depth = MTL::PixelFormatInvalid;
        bool stencil = false;
        int samples = 1;
    };

    struct MetalBlendState
    {

    };

    class MetalRenderPipeline final : public MetalPipeline, public RenderPipelineBase
    {
    public:
        explicit MetalRenderPipeline(const MetalGraphicsDevice* device);

        ~MetalRenderPipeline();

        [[nodiscard]] MTL::RenderPipelineState* raw() const { return _pipeline; }

        /// The pipelines this cache holds.
        [[nodiscard]] int count() const
        {
            size_t total = 0;
            for (const auto& [hash, entries] : _cache) {
                total += entries.size();
            }
            return static_cast<int>(total);
        }

        // Get or create a render pipeline with the specified parameters
        MTL::RenderPipelineState* get(const Primitive& primitive, const std::shared_ptr<VertexFormat>& vertexFormat0,
            const std::shared_ptr<VertexFormat>& vertexFormat1, int ibFormat, const std::shared_ptr<Shader>& shader,
            const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
            const std::shared_ptr<BlendState>& blendState, const std::shared_ptr<DepthState>& depthState,
            CullMode cullMode, bool stencilEnabled,
            const std::shared_ptr<StencilParameters>& stencilFront, const std::shared_ptr<StencilParameters>& stencilBack,
            const std::shared_ptr<VertexFormat>& instancingFormat = nullptr,
            // Attachment-format fingerprint of renderTarget (0 for the back buffer).
            // Supplied by the caller because it only changes when the bound target
            // does, while this runs per draw call — see MetalRenderTarget::formatKey.
            uint32_t renderTargetFormatKey = 0);

        /// As get(), for a precompile pass: a pipeline the cache does not hold is built on a
        /// compile thread, and the call returns at once. A later get() of it waits for it.
        void request(const Primitive& primitive, const std::shared_ptr<VertexFormat>& vertexFormat0,
            const std::shared_ptr<VertexFormat>& vertexFormat1, int ibFormat, const std::shared_ptr<Shader>& shader,
            const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
            const std::shared_ptr<BlendState>& blendState,
            const std::shared_ptr<VertexFormat>& instancingFormat, uint32_t renderTargetFormatKey);

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

    private:
        MTL::RenderPipelineState* lookup(const Primitive& primitive, const std::shared_ptr<VertexFormat>& vertexFormat0,
            const std::shared_ptr<VertexFormat>& vertexFormat1, int ibFormat, const std::shared_ptr<Shader>& shader,
            const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
            const std::shared_ptr<BlendState>& blendState,
            const std::shared_ptr<VertexFormat>& instancingFormat, uint32_t renderTargetFormatKey, bool async);

        MetalPipelineTargetFormats targetFormatsOf(const std::shared_ptr<RenderTarget>& renderTarget) const;

        // Builds a render pipeline; safe on a compile thread (it reads only its arguments,
        // the shader's library and the device).
        MTL::RenderPipelineState* create(
            const std::shared_ptr<Shader>& shader,
            const MetalPipelineTargetFormats& targetFormats,
            const BlendState& blendState,
            int vertexStride = 56,
            int instancingStride = 0
        );

        // Set the blend state configuration
        static MTL::ColorWriteMask writeMaskOf(const BlendState& blendState);
        void setBlend(MTL::RenderPipelineColorAttachmentDescriptor* colorAttachment, const BlendState& blendState);

        MTL::RenderPipelineState* _pipeline;

        // Temporary array for hash lookups
        std::vector<uint32_t> _lookupHashes;

        // The last few lookups and their answers. Consecutive draws nearly always ask for
        // the same pipeline, or alternate between two or three (a UI draws image, text,
        // image, text), and comparing fifteen words against a recent key is cheaper than
        // hashing them and walking the cache. The cache never evicts, so an answer stays
        // valid for as long as this object lives.
        static constexpr size_t kLookupKeyWords = 15;
        static constexpr size_t kRecentLookups = 4;
        struct RecentLookup
        {
            std::array<uint32_t, kLookupKeyWords> key{};
            MTL::RenderPipelineState* pipeline = nullptr;
        };
        void rememberLookup(MTL::RenderPipelineState* pipeline);
        std::array<RecentLookup, kRecentLookups> _recentLookups;
        // The most recent entry, tried first; the next write goes to the one after it.
        size_t _recentLookup = 0;

        // The cache of render pipelines
        std::unordered_map<uint32_t, std::vector<std::shared_ptr<CacheEntry>>> _cache;

        // The compile threads of request(), made on the first, and how a get() waits for an
        // entry still building.
        std::unique_ptr<PipelineCompileQueue> _compileQueue;
        std::mutex _readyMutex;
        std::condition_variable _readyCondition;
        int _requestCount = 0;
        std::atomic<int> _labelId{0};

        // The cache of vertex buffer layouts

        // Mapping tables
        static const MTL::PrimitiveType primitiveTopology[5];
        static const MTL::BlendOperation blendOperation[5];
        static const MTL::BlendFactor blendFactor[17];
    };
}
