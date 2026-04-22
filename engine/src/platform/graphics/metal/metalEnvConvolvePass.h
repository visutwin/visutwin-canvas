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

    struct EnvConvolvePassParams;
    struct EnvConvolveOp;

    // Importance-sampled environment-atlas convolution (Lambert / GGX). All
    // rects must be drawn inside ONE render pass — same constraint as
    // MetalEnvReprojectPass on Apple-Silicon tile-based GPUs.
    class MetalEnvConvolvePass
    {
    public:
        MetalEnvConvolvePass(MetalGraphicsDevice* device, MetalComposePass* composePass);
        ~MetalEnvConvolvePass();

        void beginPass(MTL::RenderCommandEncoder* encoder,
            Texture* sourceEquirect,
            Texture* sourceCubemap,
            MetalRenderPipeline* pipeline,
            const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats);

        // Allocates a transient MTL::Buffer for op.samples, submits the draw.
        // The buffer is retained via the command buffer until completion.
        void drawRect(MTL::RenderCommandEncoder* encoder,
            const EnvConvolveOp& op,
            bool sourceIsCubemap,
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
