// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Volumetric fog passes: the reduced-resolution ray-march, and the depth-aware upsample that
// composites the result over the scene.
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

    struct VolumetricFogPassParams;
    struct VolumetricFogCombineParams;

    /**
     * Manages both volumetric fog passes: lazy resource creation, pipeline lookup, and dispatch.
     * Depends on MetalComposePass for the shared full-screen vertex buffer/format.
     */
    class MetalVolumetricFogPass
    {
    public:
        MetalVolumetricFogPass(MetalGraphicsDevice* device, MetalComposePass* composePass);
        ~MetalVolumetricFogPass();

        /// Ray-march the fog into the bound (reduced-resolution) render target.
        void execute(MTL::RenderCommandEncoder* encoder,
            const VolumetricFogPassParams& params,
            MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
            MTL::SamplerState* defaultSampler, MTL::DepthStencilState* defaultDepthStencilState);

        /// Upsample the fog texture and blend it over the bound scene render target.
        void executeCombine(MTL::RenderCommandEncoder* encoder,
            const VolumetricFogCombineParams& params,
            MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
            MTL::SamplerState* defaultSampler, MTL::DepthStencilState* defaultDepthStencilState);

    private:
        void ensureResources();

        MetalGraphicsDevice* _device;
        MetalComposePass* _composePass;

        std::shared_ptr<Shader> _fogShader;
        std::shared_ptr<Shader> _combineShader;
        std::shared_ptr<BlendState> _blendState;
        // The combine pass blends as `scene * transmittance + inscatter`, i.e. src ONE,
        // dst SRC_ALPHA.
        std::shared_ptr<BlendState> _combineBlendState;
        std::shared_ptr<DepthState> _depthState;
        MTL::DepthStencilState* _depthStencilState = nullptr;
    };
}
