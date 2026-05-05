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
    };
}
