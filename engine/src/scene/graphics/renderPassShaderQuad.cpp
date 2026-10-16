// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#include "renderPassShaderQuad.h"

#include "scene/shader-lib/slangShaders.h"

#include "quadRender.h"
#include "quadShader.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    void RenderPassShaderQuad::setShader(const std::shared_ptr<Shader>& shader)
    {
        // A pass that resolves its shader every frame (compose) mostly gets the same one.
        if (shader == _shader && _quadRender) {
            return;
        }

        // destroy old
        _quadRender.reset();

        // handle new
        _shader = shader;
        if (_shader) {
            _quadRender = std::make_shared<QuadRender>(_shader);
        }
    }

    void RenderPassShaderQuad::useSlangShader(const std::string& program, const std::string& variant,
        const ShaderFeatureSet& features)
    {
        const auto gd = device();
        if (!gd) {
            return;
        }
        setShader(getOrCreateSlangShader(gd.get(), program, variant, features));
    }

    void RenderPassShaderQuad::useSlangSource(const std::string& name, const std::string& source,
        const std::string& variant)
    {
        const auto gd = device();
        if (!gd) {
            return;
        }
        setShader(getOrCreateSlangShaderFromSource(gd.get(), name, source, variant));
    }

    void RenderPassShaderQuad::useCachedShader(const char* cacheKey, const char* vertexEntry,
        const char* fragmentEntry, const char* msl, const char* glsl)
    {
        const auto gd = device();
        if (!gd) {
            return;
        }
        setShader(getOrCreateQuadShader(gd.get(), cacheKey, vertexEntry, fragmentEntry, msl, glsl));
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
            _quadRender->setGeometry(_geometry, _geometryVertexCount);
            _quadRender->render(viewportPtr, scissorPtr);
        }
    }
}
