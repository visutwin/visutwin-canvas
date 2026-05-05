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
        params.sceneTexture = _sceneTexture;
        params.bloomTexture = _bloomTexture;
        params.cocTexture = _cocTexture;
        params.blurTexture = _blurTexture;
        params.ssaoTexture = _ssaoTexture;
        params.bloomIntensity = _bloomIntensity;
        params.dofIntensity = _dofIntensity;
        params.dofEnabled = _dofEnabled;
        params.taaEnabled = _taaEnabled;
        params.blurTextureUpscale = _blurTextureUpscale;
        params.sharpness = _sharpness;
        params.toneMapping = _toneMapping;
        params.exposure = _exposure;
        // Single-pass DOF
        params.depthTexture = _depthTexture;
        params.dofFocusDistance = _dofFocusDistance;
        params.dofFocusRange = _dofFocusRange;
        params.dofBlurRadius = _dofBlurRadius;
        params.dofCameraNear = _dofCameraNear;
        params.dofCameraFar = _dofCameraFar;

        params.vignetteEnabled = _vignetteEnabled;
        params.vignetteInner = _vignetteInner;
        params.vignetteOuter = _vignetteOuter;
        params.vignetteCurvature = _vignetteCurvature;
        params.vignetteIntensity = _vignetteIntensity;
        gd->executeComposePass(params);
    }
}
