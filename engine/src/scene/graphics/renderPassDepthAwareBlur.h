// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include <memory>

#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class CameraComponent;

    /**
     * Render pass implementing a bilateral depth-aware blur. Used to blur
     * the SSAO output while preserving edges at depth discontinuities.
     */
    class RenderPassDepthAwareBlur : public RenderPassShaderQuad
    {
    public:
        RenderPassDepthAwareBlur(const std::shared_ptr<GraphicsDevice>& device,
            Texture* sourceTexture, CameraComponent* cameraComponent, bool horizontal);

        void execute() override;

    private:
        Texture* _sourceTexture = nullptr;
        CameraComponent* _cameraComponent = nullptr;
        bool _horizontal = true;
    };
}
