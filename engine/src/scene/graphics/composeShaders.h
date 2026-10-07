// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// The uniform block of the compose pass, engine/shaders/slang/programs/compose.slang.
//
// Chain order:
//   CAS -> DOF -> SSAO -> Fringing -> Bloom -> ColorEnhance -> Grading
//   -> ToneMap -> ColorLUT -> Vignette -> display gamma
//
// Textures (quad slots, both backends): 0 scene, 1 bloom, 2 ssao, 3 depth,
// 4 colorLUT, 5 colorLUT2, 6 coc, 7 dof blur (the last two only with the
// multi-pass DOF pipeline; dofMultipass says which).
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::compose_shaders
{
    struct alignas(16) ComposeUniforms
    {
        uint32_t dofEnabled = 0u;
        uint32_t taaEnabled = 0u;
        uint32_t ssaoEnabled = 0u;
        uint32_t bloomEnabled = 0u;
        uint32_t blurTextureUpscale = 0u;
        float bloomIntensity = 0.01f;
        float dofIntensity = 1.0f;
        float sharpness = 0.0f;
        uint32_t tonemapMode = 0u;
        float exposure = 1.0f;
        float sceneTextureInvRes[2] = {0.0f, 0.0f};
        // Single-pass DOF parameters
        float dofFocusDistance = 1.0f;
        float dofFocusRange = 0.5f;
        float dofBlurRadius = 3.0f;
        float dofCameraNear = 0.01f;
        float dofCameraFar = 100.0f;
        // How many source texels across map to one output pixel: the camera
        // frame's render target scale. One means the scene was rendered at the
        // output resolution. Sits where alignment padding would otherwise be, so it
        // costs the block no size.
        float sceneDownscale = 1.0f;
        // Vignette
        uint32_t vignetteEnabled = 0u;
        float vignetteInner = 0.5f;
        float vignetteOuter = 1.0f;
        float vignetteCurvature = 0.5f;
        float vignetteIntensity = 0.3f;
        float vignetteColorR = 0.0f;
        float vignetteColorG = 0.0f;
        float vignetteColorB = 0.0f;
        // Fringing
        float fringingIntensity = 0.0f;
        // Color grading
        uint32_t gradingEnabled = 0u;
        float gradingBrightness = 1.0f;
        float gradingContrast = 1.0f;
        float gradingSaturation = 1.0f;
        float gradingTintR = 1.0f;
        float gradingTintG = 1.0f;
        float gradingTintB = 1.0f;
        // Color enhance
        uint32_t colorEnhanceEnabled = 0u;
        float ceShadows = 0.0f;
        float ceHighlights = 0.0f;
        float ceVibrance = 0.0f;
        float ceDehaze = 0.0f;
        float ceMidtones = 0.0f;
        // Color LUT
        uint32_t lutEnabled = 0u;
        uint32_t lut2Enabled = 0u;
        float lutIntensity1 = 1.0f;
        float lutIntensity2 = 1.0f;
        float lutBlend = 0.0f;
        // 1 = read the CoC and blur textures (multi-pass DOF), 0 = single-pass depth blur.
        uint32_t dofMultipass = 0u;
        // Texel size of the DOF blur texture, for the low-quality 3x3 upsample.
        float dofBlurTexelX = 0.0f;
        float dofBlurTexelY = 0.0f;
    };

    // A block of scalars (plus one vec2) packs identically under MSL and
    // std140, so both shaders declare this same field list.
    static_assert(sizeof(ComposeUniforms) <= 512, "must fit kPerDrawUniformCapacity");

}
