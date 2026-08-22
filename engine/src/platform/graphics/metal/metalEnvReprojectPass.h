// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
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
    class MetalComposePass;
    class MetalGraphicsDevice;
    class MetalRenderPipeline;
    class RenderTarget;
    class Shader;
    class Texture;

    struct EnvReprojectPassParams;
    struct EnvReprojectOp;

    // All rects must be drawn inside ONE render pass: LoadActionLoad with
    // partial scissor does not reliably preserve content outside the scissor
    // on Apple-Silicon tile-based GPUs.
    class MetalEnvReprojectPass
    {
    public:
        MetalEnvReprojectPass(MetalGraphicsDevice* device, MetalComposePass* composePass);
        ~MetalEnvReprojectPass();

        void beginPass(MTL::RenderCommandEncoder* encoder,
            Texture* sourceEquirect,
            Texture* sourceCubemap,
            MetalRenderPipeline* pipeline,
            const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats);

        // sourceProjection selects how the bound source is sampled (CUBE reads the
        // cubemap, EQUIRECT/OCTAHEDRAL the 2D texture); targetProjection decides which
        // direction each destination texel represents.
        void drawRect(MTL::RenderCommandEncoder* encoder,
            const EnvReprojectOp& op,
            TextureProjection sourceProjection,
            TextureProjection targetProjection,
            bool encodeRgbp,
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
