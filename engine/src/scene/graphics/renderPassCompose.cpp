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

        params.fringingIntensity = _fringingIntensity;

        params.gradingEnabled = _gradingEnabled;
        params.gradingBrightness = _gradingBrightness;
        params.gradingContrast = _gradingContrast;
        params.gradingSaturation = _gradingSaturation;
        params.gradingTint[0] = _gradingTint[0];
        params.gradingTint[1] = _gradingTint[1];
        params.gradingTint[2] = _gradingTint[2];

        params.colorEnhanceEnabled = _colorEnhanceShadows != 0.0f || _colorEnhanceHighlights != 0.0f ||
            _colorEnhanceVibrance != 0.0f || _colorEnhanceDehaze != 0.0f || _colorEnhanceMidtones != 0.0f;
        params.colorEnhanceShadows = _colorEnhanceShadows;
        params.colorEnhanceHighlights = _colorEnhanceHighlights;
        params.colorEnhanceVibrance = _colorEnhanceVibrance;
        params.colorEnhanceDehaze = _colorEnhanceDehaze;
        params.colorEnhanceMidtones = _colorEnhanceMidtones;

        params.colorLUT = _colorLUT;
        params.colorLUT2 = _colorLUT2;
        params.colorLUTIntensity = _colorLUTIntensity;
        params.colorLUTIntensity2 = _colorLUTIntensity2;
        params.colorLUTBlend = _colorLUTBlend;

        gd->executeComposePass(params);
    }
}
