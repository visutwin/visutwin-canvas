// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The compose pass's colour settings — vignette, fringing, grading, colour enhance and
// the 3D LUTs — declared once. CameraComponent::RenderingSettings and CameraFrameOptions
// both derive from it, so the camera's settings reach the compose pass by one assignment
// instead of a field-by-field copy at every hop, and a new setting is one line here plus
// its line in RenderPassCompose::execute.
//
#pragma once

namespace visutwin::canvas
{
    class Texture;

    struct ComposeColorSettings
    {
        // Vignette
        bool vignetteEnabled = false;
        float vignetteInner = 0.5f;
        float vignetteOuter = 1.0f;
        float vignetteCurvature = 0.5f;
        float vignetteIntensity = 0.3f;
        // Darkening colour. Black is the usual choice; it exists for a tinted
        // vignette.
        float vignetteColor[3] = {0.0f, 0.0f, 0.0f};

        // Fringing (chromatic aberration): user units 0..~100, 0 = disabled. The
        // shader takes intensity / 1024.
        float fringingIntensity = 0.0f;

        // Color grading (HDR, pre-tonemap); 1.0 = no change
        bool gradingEnabled = false;
        float gradingBrightness = 1.0f;
        float gradingContrast = 1.0f;
        float gradingSaturation = 1.0f;
        float gradingTint[3] = {1.0f, 1.0f, 1.0f};

        // Color enhance (pre-tonemap); 0 = no change
        float colorEnhanceShadows = 0.0f;
        float colorEnhanceHighlights = 0.0f;
        float colorEnhanceVibrance = 0.0f;
        float colorEnhanceDehaze = 0.0f;
        float colorEnhanceMidtones = 0.0f;

        // 3D color LUT (post-tonemap): 256x16 Unreal-format strip textures
        Texture* colorLUT = nullptr;
        Texture* colorLUT2 = nullptr;
        float colorLUTIntensity = 1.0f;
        float colorLUTIntensity2 = 1.0f;
        float colorLUTBlend = 0.0f;

        /// True when any setting here changes the composed image, which therefore needs
        /// the scene rendered through a camera frame for the compose pass to apply it.
        bool changesImage() const
        {
            return vignetteEnabled || fringingIntensity > 0.0f || gradingEnabled ||
                colorEnhanceShadows != 0.0f || colorEnhanceHighlights != 0.0f ||
                colorEnhanceVibrance != 0.0f || colorEnhanceDehaze != 0.0f ||
                colorEnhanceMidtones != 0.0f || colorLUT != nullptr || colorLUT2 != nullptr;
        }
    };
}
