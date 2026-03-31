// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassCompose.h"

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    void RenderPassCompose::execute()
    {
        const auto gd = device();
        if (!gd) {
            return;
        }

        ComposePassParams params;
        params.sceneTexture = sceneTexture;
        params.bloomTexture = bloomTexture;
        params.cocTexture = cocTexture;
        params.blurTexture = blurTexture;
        params.ssaoTexture = ssaoTexture;
        params.bloomIntensity = bloomIntensity;
        params.dofIntensity = dofIntensity;
        params.dofEnabled = dofEnabled;
        params.taaEnabled = taaEnabled;
        params.blurTextureUpscale = blurTextureUpscale;
        params.sharpness = sharpness;
        params.toneMapping = toneMapping;
        params.exposure = exposure;
        // Single-pass DOF
        params.depthTexture = depthTexture;
        params.dofFocusDistance = dofFocusDistance;
        params.dofFocusRange = dofFocusRange;
        params.dofBlurRadius = dofBlurRadius;
        params.dofCameraNear = dofCameraNear;
        params.dofCameraFar = dofCameraFar;

        params.vignetteEnabled = vignetteEnabled;
        params.vignetteInner = vignetteInner;
        params.vignetteOuter = vignetteOuter;
        params.vignetteCurvature = vignetteCurvature;
        params.vignetteIntensity = vignetteIntensity;
        gd->executeComposePass(params);
    }
}
