// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Circle of Confusion (CoC) pass for Depth of Field.
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

    struct CoCPassParams;

    /**
     * Manages the CoC pass: lazy resource creation, pipeline lookup, and dispatch.
     * Depends on MetalComposePass for shared full-screen vertex buffer/format.
     */
    class MetalCoCPass
    {
    public:
        MetalCoCPass(MetalGraphicsDevice* device, MetalComposePass* composePass);
        ~MetalCoCPass();

        /// Execute the CoC pass on the active render command encoder.
        void execute(MTL::RenderCommandEncoder* encoder,
            const CoCPassParams& params,
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
