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

        Texture* sceneTexture() const { return _sceneTexture; }
        void setSceneTexture(Texture* value) { _sceneTexture = value; }

        Texture* bloomTexture() const { return _bloomTexture; }
        void setBloomTexture(Texture* value) { _bloomTexture = value; }

        Texture* cocTexture() const { return _cocTexture; }
        void setCocTexture(Texture* value) { _cocTexture = value; }

        Texture* blurTexture() const { return _blurTexture; }
        void setBlurTexture(Texture* value) { _blurTexture = value; }

        Texture* ssaoTexture() const { return _ssaoTexture; }
        void setSsaoTexture(Texture* value) { _ssaoTexture = value; }

        bool taaEnabled() const { return _taaEnabled; }
        void setTaaEnabled(const bool value) { _taaEnabled = value; }

        bool blurTextureUpscale() const { return _blurTextureUpscale; }
        void setBlurTextureUpscale(const bool value) { _blurTextureUpscale = value; }

        float bloomIntensity() const { return _bloomIntensity; }
        void setBloomIntensity(const float value) { _bloomIntensity = value; }

        bool dofEnabled() const { return _dofEnabled; }
        void setDofEnabled(const bool value) { _dofEnabled = value; }

        float dofIntensity() const { return _dofIntensity; }
        void setDofIntensity(const float value) { _dofIntensity = value; }

        float sharpness() const { return _sharpness; }
        void setSharpness(const float value) { _sharpness = value; }

        int toneMapping() const { return _toneMapping; }
        void setToneMapping(const int value) { _toneMapping = value; }

        float exposure() const { return _exposure; }
        void setExposure(const float value) { _exposure = value; }

        // Single-pass DOF
        Texture* depthTexture() const { return _depthTexture; }
        void setDepthTexture(Texture* value) { _depthTexture = value; }

        float dofFocusDistance() const { return _dofFocusDistance; }
        void setDofFocusDistance(const float value) { _dofFocusDistance = value; }

        float dofFocusRange() const { return _dofFocusRange; }
        void setDofFocusRange(const float value) { _dofFocusRange = value; }

        float dofBlurRadius() const { return _dofBlurRadius; }
        void setDofBlurRadius(const float value) { _dofBlurRadius = value; }

        float dofCameraNear() const { return _dofCameraNear; }
        void setDofCameraNear(const float value) { _dofCameraNear = value; }

        float dofCameraFar() const { return _dofCameraFar; }
        void setDofCameraFar(const float value) { _dofCameraFar = value; }

        // Vignette
        bool vignetteEnabled() const { return _vignetteEnabled; }
        void setVignetteEnabled(const bool value) { _vignetteEnabled = value; }

        float vignetteInner() const { return _vignetteInner; }
        void setVignetteInner(const float value) { _vignetteInner = value; }

        float vignetteOuter() const { return _vignetteOuter; }
        void setVignetteOuter(const float value) { _vignetteOuter = value; }

        float vignetteCurvature() const { return _vignetteCurvature; }
        void setVignetteCurvature(const float value) { _vignetteCurvature = value; }

        float vignetteIntensity() const { return _vignetteIntensity; }
        void setVignetteIntensity(const float value) { _vignetteIntensity = value; }

        const float* vignetteColor() const { return _vignetteColor; }
        void setVignetteColor(const float r, const float g, const float b)
        {
            _vignetteColor[0] = r;
            _vignetteColor[1] = g;
            _vignetteColor[2] = b;
        }

        // Fringing (chromatic aberration); shader units (user value / 1024)
        void setFringingIntensity(const float value) { _fringingIntensity = value; }

        // Color grading (HDR, pre-tonemap)
        void setGradingEnabled(const bool value) { _gradingEnabled = value; }
        void setGradingBrightness(const float value) { _gradingBrightness = value; }
        void setGradingContrast(const float value) { _gradingContrast = value; }
        void setGradingSaturation(const float value) { _gradingSaturation = value; }
        void setGradingTint(const float r, const float g, const float b)
        {
            _gradingTint[0] = r; _gradingTint[1] = g; _gradingTint[2] = b;
        }

        // Color enhance (pre-tonemap)
        void setColorEnhance(const float shadows, const float highlights, const float vibrance,
                             const float dehaze, const float midtones)
        {
            _colorEnhanceShadows = shadows;
            _colorEnhanceHighlights = highlights;
            _colorEnhanceVibrance = vibrance;
            _colorEnhanceDehaze = dehaze;
            _colorEnhanceMidtones = midtones;
        }

        // 3D color LUT (post-tonemap): 256x16 Unreal-format strip textures
        void setColorLUT(Texture* lut, const float intensity = 1.0f)
        {
            _colorLUT = lut;
            _colorLUTIntensity = intensity;
        }
        void setColorLUT2(Texture* lut, const float intensity, const float blend)
        {
            _colorLUT2 = lut;
            _colorLUTIntensity2 = intensity;
            _colorLUTBlend = blend;
        }

        void execute() override;

    private:
        Texture* _sceneTexture = nullptr;
        Texture* _bloomTexture = nullptr;
        Texture* _cocTexture = nullptr;
        Texture* _blurTexture = nullptr;
        Texture* _ssaoTexture = nullptr;
        bool _taaEnabled = false;
        bool _blurTextureUpscale = false;
        float _bloomIntensity = 0.01f;
        bool _dofEnabled = false;
        float _dofIntensity = 1.0f;
        float _sharpness = 0.0f;
        int _toneMapping = 0;
        float _exposure = 1.0f;

        Texture* _depthTexture = nullptr;
        float _dofFocusDistance = 1.0f;
        float _dofFocusRange = 0.5f;
        float _dofBlurRadius = 3.0f;
        float _dofCameraNear = 0.01f;
        float _dofCameraFar = 100.0f;

        bool _vignetteEnabled = false;
        float _vignetteInner = 0.5f;
        float _vignetteOuter = 1.0f;
        float _vignetteCurvature = 0.5f;
        float _vignetteIntensity = 0.3f;
        float _vignetteColor[3] = {0.0f, 0.0f, 0.0f};

        float _fringingIntensity = 0.0f;

        bool _gradingEnabled = false;
        float _gradingBrightness = 1.0f;
        float _gradingContrast = 1.0f;
        float _gradingSaturation = 1.0f;
        float _gradingTint[3] = {1.0f, 1.0f, 1.0f};

        float _colorEnhanceShadows = 0.0f;
        float _colorEnhanceHighlights = 0.0f;
        float _colorEnhanceVibrance = 0.0f;
        float _colorEnhanceDehaze = 0.0f;
        float _colorEnhanceMidtones = 0.0f;

        Texture* _colorLUT = nullptr;
        Texture* _colorLUT2 = nullptr;
        float _colorLUTIntensity = 1.0f;
        float _colorLUTIntensity2 = 1.0f;
        float _colorLUTBlend = 0.0f;
    };
}
