// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Separable gaussian blur for VSM/EVSM moments (RGB channels of an RGBA16F texture).
// Mirrors upstream blurVSM.js — one direction per dispatch, two passes total.
//
#pragma once

#include <memory>
#include <vector>
#include <Metal/Metal.hpp>

#include "platform/graphics/graphicsDevice.h"   // VsmBlurPassParams

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

    /**
     * Lazy resource creation, single-direction gaussian blur dispatch.
     * Instantiated twice (horizontal + vertical) to form a separable 2D blur.
     */
    class MetalVsmBlurPass
    {
    public:
        MetalVsmBlurPass(MetalGraphicsDevice* device, MetalComposePass* composePass, bool horizontal);
        ~MetalVsmBlurPass();

        void execute(MTL::RenderCommandEncoder* encoder,
            const VsmBlurPassParams& params,
            MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
            MTL::DepthStencilState* defaultDepthStencilState);

    private:
        void ensureResources();

        MetalGraphicsDevice* _device;
        MetalComposePass* _composePass;
        bool _horizontal;

        std::shared_ptr<Shader> _shader;
        std::shared_ptr<BlendState> _blendState;
        std::shared_ptr<DepthState> _depthState;
        MTL::DepthStencilState* _depthStencilState = nullptr;
        MTL::SamplerState* _linearSampler = nullptr;
    };
}
