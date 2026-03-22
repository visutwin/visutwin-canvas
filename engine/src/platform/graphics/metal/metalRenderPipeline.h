// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.07.2025.
//
#pragma once

#include "Metal/Metal.hpp"
#include "metalGraphicsDevice.h"
#include "metalPipeline.h"
#include "metalVertexBufferLayout.h"
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
            const std::shared_ptr<VertexFormat>& instancingFormat = nullptr);

    private:
        // Create a new render pipeline
        MTL::RenderPipelineState* create(
            const MTL::PrimitiveType primitiveTopology, int ibFormat, const std::shared_ptr<Shader>& shader,
            const std::shared_ptr<RenderTarget>& renderTarget, metal::PipelineLayout* pipelineLayout,
            std::shared_ptr<BlendState> blendState, std::shared_ptr<DepthState> depthState,
            const std::vector<void*>& vertexBufferLayout, CullMode cullMode, bool stencilEnabled,
            std::shared_ptr<StencilParameters> stencilFront, std::shared_ptr<StencilParameters> stencilBack,
            int vertexStride = 56,
            int instancingStride = 0
        );

        // Set the blend state configuration
        void setBlend(MTL::RenderPipelineColorAttachmentDescriptor* colorAttachment, const std::shared_ptr<BlendState>& blendState);

        MTL::RenderPipelineState* _pipeline;

        // Temporary array for hash lookups
        std::vector<uint32_t> _lookupHashes;

        // The cache of render pipelines
        std::unordered_map<uint32_t, std::vector<std::shared_ptr<CacheEntry>>> _cache;

        // The cache of vertex buffer layouts
        std::unique_ptr<MetalVertexBufferLayout> _vertexBufferLayout;

        // Mapping tables
        static const MTL::PrimitiveType primitiveTopology[5];
        static const MTL::BlendOperation blendOperation[5];
        static const MTL::BlendFactor blendFactor[13];
    };
}
