// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#include "renderPassDepthAwareBlur.h"

#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    namespace
    {
        struct alignas(16) BlurUniforms
        {
            float invResAndDir[4];  // xy = 1/resolution, zw = blur direction
            float params[4];        // x = filterSize, y = cameraNear, z = cameraFar
        };
        static_assert(sizeof(BlurUniforms) == 32);

    }

    RenderPassDepthAwareBlur::RenderPassDepthAwareBlur(const std::shared_ptr<GraphicsDevice>& device,
        Texture* sourceTexture, CameraComponent* cameraComponent, const bool horizontal)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture),
          _cameraComponent(cameraComponent), _horizontal(horizontal)
    {
    }

    void RenderPassDepthAwareBlur::prepareShaders()
    {
        if (!shader()) {
            useSlangShader("depth-aware-blur");
        }
    }

    void RenderPassDepthAwareBlur::execute()
    {
        const auto gd = device();
        if (!gd || !_sourceTexture || !_cameraComponent || !_cameraComponent->camera()) {
            return;
        }

        Texture* depthTexture = gd->sceneDepthMap();
        if (!depthTexture) {
            return;
        }

        const auto rt = renderTarget();
        if (!rt || !rt->colorBuffer()) {
            return;
        }

        const auto* camera = _cameraComponent->camera();

        // Normally there already: the frame graph prepares every pass's shaders first.
        prepareShaders();
        if (!shader()) {
            return;
        }

        BlurUniforms uniforms{};
        uniforms.invResAndDir[0] = 1.0f / static_cast<float>(rt->colorBuffer()->width());
        uniforms.invResAndDir[1] = 1.0f / static_cast<float>(rt->colorBuffer()->height());
        uniforms.invResAndDir[2] = _horizontal ? 1.0f : 0.0f;
        uniforms.invResAndDir[3] = _horizontal ? 0.0f : 1.0f;
        uniforms.params[0] = 8.0f;  // filterSize
        uniforms.params[1] = camera->nearClip();
        uniforms.params[2] = camera->farClip();

        setQuadTextureBinding(0, _sourceTexture);
        setQuadTextureBinding(1, depthTexture);
        setQuadUniforms(uniforms);
        RenderPassShaderQuad::execute();
    }
}
