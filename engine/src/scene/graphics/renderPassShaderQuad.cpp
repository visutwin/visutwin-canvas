// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassShaderQuad.h"

#include "quadRender.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    void RenderPassShaderQuad::setShader(const std::shared_ptr<Shader>& shader)
    {
        // destroy old
        _quadRender.reset();

        // handle new
        _shader = shader;
        if (_shader) {
            _quadRender = std::make_shared<QuadRender>(_shader);
        }
    }

    void RenderPassShaderQuad::execute()
    {
        const auto gd = device();
        if (!gd) {
            return;
        }

        // render state
        gd->setBlendState(blendState);
        gd->setCullMode(cullMode);
        gd->setDepthState(depthState);
        gd->setStencilState(stencilFront, stencilBack);
        gd->clearQuadTextureBindings();
        for (size_t i = 0; i < _quadTextureBindings.size(); ++i) {
            gd->setQuadTextureBinding(i, _quadTextureBindings[i]);
        }

        const Vector4* viewportPtr = viewport ? &(*viewport) : nullptr;
        const Vector4* scissorPtr = scissor ? &(*scissor) : nullptr;
        if (_quadRender) {
            _quadRender->render(viewportPtr, scissorPtr);
        }
    }
}
