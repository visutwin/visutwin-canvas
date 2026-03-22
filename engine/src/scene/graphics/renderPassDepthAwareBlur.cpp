// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassDepthAwareBlur.h"

#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    RenderPassDepthAwareBlur::RenderPassDepthAwareBlur(const std::shared_ptr<GraphicsDevice>& device,
        Texture* sourceTexture, CameraComponent* cameraComponent, const bool horizontal)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture),
          _cameraComponent(cameraComponent), _horizontal(horizontal)
    {
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

        DepthAwareBlurPassParams params;
        params.sourceTexture = _sourceTexture;
        params.depthTexture = depthTexture;
        params.filterSize = 8;
        params.sourceInvResolutionX = 1.0f / static_cast<float>(rt->colorBuffer()->width());
        params.sourceInvResolutionY = 1.0f / static_cast<float>(rt->colorBuffer()->height());
        params.cameraNear = camera->nearClip();
        params.cameraFar = camera->farClip();

        gd->executeDepthAwareBlurPass(params, _horizontal);
    }
}
