// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "renderPassVsmBlur.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"

namespace visutwin::canvas
{
    RenderPassVsmBlur::RenderPassVsmBlur(const std::shared_ptr<GraphicsDevice>& device,
        Texture* sourceTexture,
        const std::shared_ptr<RenderTarget>& targetRenderTarget,
        const int shadowResolution,
        const bool horizontal,
        const int filterSize)
        : RenderPass(device),
          _sourceTexture(sourceTexture),
          _shadowResolution(shadowResolution),
          _horizontal(horizontal),
          _filterSize(filterSize)
    {
        _requiresCubemaps = false;
        _name = horizontal ? "RenderPassVsmBlurH" : "RenderPassVsmBlurV";

        init(targetRenderTarget);
        // Full overwrite — no clear required since we touch every pixel of the rect.
        if (colorOps()) {
            colorOps()->clear = false;
        }
        if (depthStencilOps()) {
            depthStencilOps()->clearDepth = false;
            depthStencilOps()->storeDepth = false;
        }
    }

    void RenderPassVsmBlur::execute()
    {
        auto dev = device();
        if (!dev || !_sourceTexture || _shadowResolution <= 0) {
            return;
        }

        VsmBlurPassParams params;
        params.sourceTexture = _sourceTexture;
        params.sourceInvResolutionX = 1.0f / static_cast<float>(_shadowResolution);
        params.sourceInvResolutionY = 1.0f / static_cast<float>(_shadowResolution);
        params.filterSize = _filterSize;
        dev->executeVsmBlurPass(params, _horizontal);
    }
}
