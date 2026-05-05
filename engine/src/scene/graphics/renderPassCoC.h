// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include "framework/components/camera/cameraComponent.h"
#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class RenderPassCoC : public RenderPassShaderQuad
    {
    public:
        RenderPassCoC(const std::shared_ptr<GraphicsDevice>& device, CameraComponent* cameraComponent, bool nearBlur);

        float focusDistance() const { return _focusDistance; }
        void setFocusDistance(const float value) { _focusDistance = value; }

        float focusRange() const { return _focusRange; }
        void setFocusRange(const float value) { _focusRange = value; }

        void execute() override;

    private:
        CameraComponent* _cameraComponent = nullptr;
        bool _nearBlur = false;
        float _focusDistance = 100.0f;
        float _focusRange = 10.0f;
        float _params[3] = {100.001f, 10.0f, 0.1f};
        float _cameraParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };
}
