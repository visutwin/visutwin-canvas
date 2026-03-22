// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassCoC.h"

#include <algorithm>

#include "scene/camera.h"

namespace visutwin::canvas
{
    RenderPassCoC::RenderPassCoC(const std::shared_ptr<GraphicsDevice>& device, CameraComponent* cameraComponent,
        const bool nearBlur)
        : RenderPassShaderQuad(device), _cameraComponent(cameraComponent), _nearBlur(nearBlur)
    {
    }

    void RenderPassCoC::execute()
    {
        _params[0] = focusDistance + 0.001f;
        _params[1] = std::max(focusRange, 0.001f);
        _params[2] = 1.0f / _params[1];

        if (const auto* camera = _cameraComponent ? _cameraComponent->camera() : nullptr) {
            _cameraParams[0] = camera->nearClip();
            _cameraParams[1] = camera->farClip();
            _cameraParams[2] = camera->projection() == ProjectionType::Perspective ? 1.0f : 0.0f;
            _cameraParams[3] = _nearBlur ? 1.0f : 0.0f;
        }

        RenderPassShaderQuad::execute();
    }
}

