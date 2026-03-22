// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Compose post-processing pass — CAS sharpening, SSAO, DOF, bloom, tonemapping, gamma.
// Extracted from MetalGraphicsDevice for single-responsibility decomposition.
//
#pragma once

#include <memory>
#include <vector>
#include <Metal/Metal.hpp>

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    class BlendState;
    class DepthState;
    class MetalBindGroupFormat;
    class MetalGraphicsDevice;
    class MetalRenderPipeline;
    class RenderTarget;
    class Shader;
    class VertexBuffer;
    class VertexFormat;

    /**
     * Manages the full-screen compose pass: lazy resource creation, pipeline lookup, and dispatch.
     * Owns the compose shader source, vertex buffer (shared with MetalTaaPass), blend/depth states.
     */
    class MetalComposePass
    {
    public:
        explicit MetalComposePass(MetalGraphicsDevice* device);
        ~MetalComposePass();

        /// Execute the compose pass on the active render command encoder.
        void execute(MTL::RenderCommandEncoder* encoder, const ComposePassParams& params,
            MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
            MTL::SamplerState* defaultSampler);

        /// Shared vertex format (full-screen triangle, 14 floats per vertex).
        [[nodiscard]] std::shared_ptr<VertexFormat> vertexFormat() const { return _vertexFormat; }

        /// Shared vertex buffer (3-vertex full-screen triangle).
        [[nodiscard]] std::shared_ptr<VertexBuffer> vertexBuffer() const { return _vertexBuffer; }

        /// Initialize shared resources (vertex buffer, format, etc.) if not already done.
        /// Called by dependent passes (SSAO, blur) that share the vertex buffer/format.
        void ensureResources();

    private:

        MetalGraphicsDevice* _device;

        std::shared_ptr<Shader> _shader;
        std::shared_ptr<VertexFormat> _vertexFormat;
        std::shared_ptr<VertexBuffer> _vertexBuffer;
        std::shared_ptr<BlendState> _blendState;
        std::shared_ptr<DepthState> _depthState;
        MTL::DepthStencilState* _depthStencilState = nullptr;
    };
}
