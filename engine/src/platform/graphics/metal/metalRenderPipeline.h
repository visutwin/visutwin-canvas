// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.07.2025.
//
#pragma once

#include <array>
#include "Metal/Metal.hpp"
#include "metalGraphicsDevice.h"
#include "metalPipeline.h"
#include "platform/graphics/bindGroupFormat.h"
#include "platform/graphics/blendState.h"
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
        // Render pipeline
        MTL::RenderPipelineState* pipeline = nullptr;

        /** The full array of hashes used to look up the pipeline, used in case of hash collision */
        std::vector<uint32_t> hashes;
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

    private:
        // Create a new render pipeline
        MTL::RenderPipelineState* create(
            const std::shared_ptr<Shader>& shader,
            const std::shared_ptr<RenderTarget>& renderTarget,
            const std::shared_ptr<BlendState>& blendState,
            int vertexStride = 56,
            int instancingStride = 0
        );

        // Set the blend state configuration
        void setBlend(MTL::RenderPipelineColorAttachmentDescriptor* colorAttachment, const std::shared_ptr<BlendState>& blendState);

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

        // The cache of vertex buffer layouts

        // Mapping tables
        static const MTL::PrimitiveType primitiveTopology[5];
        static const MTL::BlendOperation blendOperation[5];
        static const MTL::BlendFactor blendFactor[17];
    };
}
