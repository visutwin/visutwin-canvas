// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassCoC.h"

#include <algorithm>

#include "scene/camera.h"
#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    RenderPassCoC::RenderPassCoC(const std::shared_ptr<GraphicsDevice>& device, CameraComponent* cameraComponent,
        const bool nearBlur)
        : RenderPassShaderQuad(device), _cameraComponent(cameraComponent), _nearBlur(nearBlur)
    {
    }

    void RenderPassCoC::execute()
    {
        _params[0] = _focusDistance + 0.001f;
        _params[1] = std::max(_focusRange, 0.001f);
        _params[2] = 1.0f / _params[1];

        const auto* camera = _cameraComponent ? _cameraComponent->camera() : nullptr;
        if (!camera) return;

        const auto gd = device();
        if (!gd) return;

        // Get depth texture from the graphics device (same mechanism as SSAO)
        Texture* depthTexture = gd->sceneDepthMap();
        if (!depthTexture) {
            // Fall back to shader quad path if no depth texture available
            if (camera) {
                _cameraParams[0] = camera->nearClip();
                _cameraParams[1] = camera->farClip();
                _cameraParams[2] = camera->projection() == ProjectionType::Perspective ? 1.0f : 0.0f;
                _cameraParams[3] = _nearBlur ? 1.0f : 0.0f;
            }
            RenderPassShaderQuad::execute();
            return;
        }

        CoCPassParams params;
        params.depthTexture = depthTexture;
        params.focusDistance = _focusDistance;
        params.focusRange = std::max(_focusRange, 0.001f);
        params.cameraNear = camera->nearClip();
        params.cameraFar = camera->farClip();
        params.nearBlur = _nearBlur;
        gd->executeCoCPass(params);
    }
}

