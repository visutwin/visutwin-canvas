// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.04.2026
//
// Separable 1D gaussian blur for EVSM moments. Operates on the RGB channels of an RGBA16F source; the alpha
// "rendered" flag is preserved by re-emitting 1.0.
//
// This effect lives ABOVE GraphicsDevice: it is a shader, one input texture and
// one uniform block driven through QuadRender, so there is a single
// implementation instead of one per backend. Direction is a uniform rather than
// two compiled variants — the same choice the debug passes make.
//
#include "renderPassVsmBlur.h"

#include "scene/shader-lib/slangShaders.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/quadRender.h"

namespace visutwin::canvas
{
    namespace
    {
        // Uniform block shared by both languages. Two vec4s keeps std140 and MSL
        // layouts identical without padding rules coming into it.
        struct VsmBlurUniforms
        {
            float invResolutionAndDirection[4];  // xy = 1/resolution, zw = blur direction
            float params[4];                     // x = filterSize, y = cascade tile size
        };

        std::shared_ptr<Shader> vsmBlurShader(GraphicsDevice* device)
        {
            return getOrCreateSlangShader(device, "vsm-blur");
        }
    }

    RenderPassVsmBlur::RenderPassVsmBlur(const std::shared_ptr<GraphicsDevice>& device,
        Texture* sourceTexture,
        const std::shared_ptr<RenderTarget>& targetRenderTarget,
        const int shadowResolution,
        const bool horizontal,
        const int filterSize,
        const float cascadeTileSize)
        : RenderPass(device),
          _sourceTexture(sourceTexture),
          _shadowResolution(shadowResolution),
          _horizontal(horizontal),
          _filterSize(filterSize),
          _cascadeTileSize(cascadeTileSize)
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

    void RenderPassVsmBlur::prepareShaders()
    {
        if (const auto dev = device()) {
            (void)vsmBlurShader(dev.get());
        }
    }

    void RenderPassVsmBlur::execute()
    {
        auto dev = device();
        if (!dev || !_sourceTexture || _shadowResolution <= 0) {
            return;
        }

        auto shader = vsmBlurShader(dev.get());
        if (!shader) {
            return;
        }

        const float invResolution = 1.0f / static_cast<float>(_shadowResolution);
        VsmBlurUniforms uniforms{};
        uniforms.invResolutionAndDirection[0] = invResolution;
        uniforms.invResolutionAndDirection[1] = invResolution;
        uniforms.invResolutionAndDirection[2] = _horizontal ? 1.0f : 0.0f;
        uniforms.invResolutionAndDirection[3] = _horizontal ? 0.0f : 1.0f;
        uniforms.params[0] = static_cast<float>(_filterSize);
        uniforms.params[1] = _cascadeTileSize;

        // The blur runs right after the shadow draws and inherits nothing from them: their
        // depth test and write would act on the temp target, which has no depth attachment,
        // and on the shadow map's depth buffer, which this pass neither clears nor stores
        // (undefined, so texels would drop out at random), and their cull mode could cull
        // the fullscreen triangle.
        dev->setBlendState(BlendState::noBlend());
        dev->setCullMode(CullMode::CULLFACE_NONE);
        dev->setDepthState(DepthState::noDepth());
        dev->setStencilState(nullptr, nullptr);

        QuadRender quad(shader);
        quad.setTexture(0, _sourceTexture);
        quad.setUniforms(uniforms);
        quad.render();
    }
}
