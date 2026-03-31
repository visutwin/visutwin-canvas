// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// DOF (Depth of Field) blur pass.
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

    struct DofBlurPassParams;

    /**
     * Manages the DOF blur pass: lazy resource creation, pipeline lookup, and dispatch.
     * Depends on MetalComposePass for shared full-screen vertex buffer/format.
     */
    class MetalDofBlurPass
    {
    public:
        MetalDofBlurPass(MetalGraphicsDevice* device, MetalComposePass* composePass);
        ~MetalDofBlurPass();

        /// Execute the DOF blur pass on the active render command encoder.
        void execute(MTL::RenderCommandEncoder* encoder,
            const DofBlurPassParams& params,
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
