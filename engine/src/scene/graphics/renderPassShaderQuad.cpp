// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
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

    void RenderPassShaderQuad::useCachedShader(const char* cacheKey, const char* vertexEntry,
        const char* fragmentEntry, const char* msl, const char* glsl)
    {
        const auto gd = device();
        if (!gd) {
            return;
        }
        auto cached = gd->getCachedShader(cacheKey);
        if (!cached) {
            ShaderDefinition definition;
            definition.name = cacheKey;
            definition.vshader = vertexEntry;
            definition.fshader = fragmentEntry;
            cached = createShader(gd.get(), definition,
                gd->shaderLanguage() == ShaderLanguage::Glsl ? glsl : msl);
            if (cached) {
                gd->setCachedShader(cacheKey, cached);
            }
        }
        setShader(cached);
    }

    void RenderPassShaderQuad::execute()
    {
        const auto gd = device();
        if (!gd) {
            return;
        }

        // render state
        gd->setBlendState(_blendState);
        gd->setCullMode(_cullMode);
        gd->setDepthState(_depthState);
        gd->setStencilState(_stencilFront, _stencilBack);
        gd->clearQuadTextureBindings();
        for (size_t i = 0; i < _quadTextureBindings.size(); ++i) {
            gd->setQuadTextureBinding(i, _quadTextureBindings[i]);
        }

        if (!_quadUniformData.empty()) {
            gd->setQuadUniformData(_quadUniformData.data(), _quadUniformData.size());
        }

        const Vector4* viewportPtr = _viewport ? &(*_viewport) : nullptr;
        const Vector4* scissorPtr = _scissor ? &(*_scissor) : nullptr;
        if (_quadRender) {
            _quadRender->render(viewportPtr, scissorPtr);
        }
    }
}
