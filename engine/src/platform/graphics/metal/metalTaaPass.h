// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// TAA (Temporal Anti-Aliasing) resolve pass.
// Extracted from MetalGraphicsDevice for single-responsibility decomposition.
//
#pragma once

#include <array>
#include <memory>
#include <vector>
#include <Metal/Metal.hpp>

namespace visutwin::canvas
{
    class BlendState;
    class DepthState;
    class Matrix4;
    class MetalBindGroupFormat;
    class MetalComposePass;
    class MetalGraphicsDevice;
    class MetalRenderPipeline;
    class RenderTarget;
    class Shader;
    class Texture;

    /**
     * Manages the TAA resolve pass: lazy resource creation, pipeline lookup, and dispatch.
     * Depends on MetalComposePass for shared full-screen vertex buffer/format.
     */
    class MetalTaaPass
    {
    public:
        MetalTaaPass(MetalGraphicsDevice* device, MetalComposePass* composePass);
        ~MetalTaaPass();

        /// Execute the TAA resolve pass on the active render command encoder.
        void execute(MTL::RenderCommandEncoder* encoder,
            Texture* sourceTexture, Texture* historyTexture, Texture* depthTexture,
            const Matrix4& viewProjectionPrevious, const Matrix4& viewProjectionInverse,
            const std::array<float, 4>& jitters, const std::array<float, 4>& cameraParams,
            bool highQuality, bool historyValid,
            MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
            MTL::SamplerState* defaultSampler, MTL::DepthStencilState* defaultDepthStencilState);

    private:
        void ensureResources();

        MetalGraphicsDevice* _device;
        MetalComposePass* _composePass;

        std::shared_ptr<Shader> _shader;
        std::shared_ptr<BlendState> _blendState;
        std::shared_ptr<DepthState> _depthState;
        MTL::DepthStencilState* _depthStencilState = nullptr;
    };
}
