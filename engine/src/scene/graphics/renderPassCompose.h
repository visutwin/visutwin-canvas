// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class RenderPassCompose : public RenderPassShaderQuad
    {
    public:
        explicit RenderPassCompose(const std::shared_ptr<GraphicsDevice>& device)
            : RenderPassShaderQuad(device) {}

        Texture* sceneTexture = nullptr;
        Texture* bloomTexture = nullptr;
        Texture* cocTexture = nullptr;
        Texture* blurTexture = nullptr;
        Texture* ssaoTexture = nullptr;
        bool taaEnabled = false;
        bool blurTextureUpscale = false;
        float bloomIntensity = 0.01f;
        bool dofEnabled = false;
        float dofIntensity = 1.0f;
        float sharpness = 0.0f;
        int toneMapping = 0;
        float exposure = 1.0f;

        // Single-pass DOF
        Texture* depthTexture = nullptr;
        float dofFocusDistance = 1.0f;
        float dofFocusRange = 0.5f;
        float dofBlurRadius = 3.0f;
        float dofCameraNear = 0.01f;
        float dofCameraFar = 100.0f;

        // Vignette
        bool vignetteEnabled = false;
        float vignetteInner = 0.5f;
        float vignetteOuter = 1.0f;
        float vignetteCurvature = 0.5f;
        float vignetteIntensity = 0.3f;

        void execute() override;
    };
}
