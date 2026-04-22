// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
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

    class MetalEquirectToCubePass
    {
    public:
        MetalEquirectToCubePass(MetalGraphicsDevice* device, MetalComposePass* composePass);
        ~MetalEquirectToCubePass();

        // Binds the pipeline, vertex buffer, source equirect, sampler on the
        // encoder. Call once per render pass (one per cube face).
        void beginPass(MTL::RenderCommandEncoder* encoder,
            Texture* sourceEquirect,
            MetalRenderPipeline* pipeline,
            const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats);

        // Draws the fullscreen triangle into the currently-bound cubemap face
        // render target. `face` (0..5) selects the face-direction mapping.
        void drawFace(MTL::RenderCommandEncoder* encoder,
            uint32_t face,
            int faceSize,
            bool decodeSrgb);

    private:
        void ensureResources();

        MetalGraphicsDevice* _device;
        MetalComposePass* _composePass;

        std::shared_ptr<Shader> _shader;
        std::shared_ptr<BlendState> _blendState;
        std::shared_ptr<DepthState> _depthState;
        MTL::DepthStencilState* _depthStencilState = nullptr;
        MTL::SamplerState* _clampSampler = nullptr;
    };
}
