// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Depth-aware bilateral blur pass.
//
#pragma once

#include <memory>
#include <vector>
#include <Metal/Metal.hpp>

namespace visutwin::canvas
{
    class BlendState;
    class DepthState;
    class MetalBindGroupFormat;
    class MetalComposePass;
    class MetalGraphicsDevice;
    class MetalRenderPipeline;
    class RenderTarget;
    class Shader;
    class Texture;

    struct DepthAwareBlurPassParams;

    /**
     * Manages a depth-aware bilateral blur pass: lazy resource creation, pipeline lookup, and dispatch.
     * Depends on MetalComposePass for shared full-screen vertex buffer/format.
     * Instantiated twice — one horizontal, one vertical.
     */
    class MetalDepthAwareBlurPass
    {
    public:
        MetalDepthAwareBlurPass(MetalGraphicsDevice* device, MetalComposePass* composePass, bool horizontal);
        ~MetalDepthAwareBlurPass();

        /// Execute the blur pass on the active render command encoder.
        void execute(MTL::RenderCommandEncoder* encoder,
            const DepthAwareBlurPassParams& params,
            MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
            MTL::SamplerState* defaultSampler, MTL::DepthStencilState* defaultDepthStencilState);

    private:
        void ensureResources();

        MetalGraphicsDevice* _device;
        MetalComposePass* _composePass;
        bool _horizontal;

        std::shared_ptr<Shader> _shader;
        std::shared_ptr<BlendState> _blendState;
        std::shared_ptr<DepthState> _depthState;
        MTL::DepthStencilState* _depthStencilState = nullptr;
    };
}
