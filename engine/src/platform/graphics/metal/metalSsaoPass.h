// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// SSAO (Screen-Space Ambient Occlusion) pass.
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

    struct SsaoPassParams;

    /**
     * Manages the SSAO pass: lazy resource creation, pipeline lookup, and dispatch.
     * Depends on MetalComposePass for shared full-screen vertex buffer/format.
     */
    class MetalSsaoPass
    {
    public:
        MetalSsaoPass(MetalGraphicsDevice* device, MetalComposePass* composePass);
        ~MetalSsaoPass();

        /// Execute the SSAO pass on the active render command encoder.
        void execute(MTL::RenderCommandEncoder* encoder,
            const SsaoPassParams& params,
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
