// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "composeColorSettings.h"
#include "composeEffects.h"
#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class CameraFrameEffect;

    class RenderPassCompose : public RenderPassShaderQuad
    {
    public:
        explicit RenderPassCompose(const std::shared_ptr<GraphicsDevice>& device)
            : RenderPassShaderQuad(device) {}

        Texture* sceneTexture() const { return _sceneTexture; }
        void setSceneTexture(Texture* value) { _sceneTexture = value; }

        /// How many source texels across map to one output pixel, i.e. the camera
        /// frame's render target scale. The compose fetch reduces over that
        /// footprint; without it a supersampled scene is point-sampled back down
        /// and the extra resolution is wasted.
        float sceneDownscale() const { return _sceneDownscale; }
        void setSceneDownscale(const float value) { _sceneDownscale = value; }

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

        /// Vignette, fringing, grading, colour enhance and the LUTs, in the camera's units
        /// (execute() converts fringing to shader units).
        const ComposeColorSettings& colorSettings() const { return _color; }
        void setColorSettings(const ComposeColorSettings& value) { _color = value; }

        /// The camera's frame effects; those active with a compose slot are composed
        /// (buildComposeEffects), each time the shader is prepared, so an effect's update()
        /// running after this is set is what the frame composes.
        void setEffects(const std::vector<std::shared_ptr<CameraFrameEffect>>& effects) { _effects = effects; }
        /// The composition as last built: the effects composed, their views, what they need.
        const ComposeEffectsBuild& effectsBuild() const { return _effectsBuild; }

        /// The debug view to show instead of the composition (composeDebugMode), empty for none.
        const std::string& debugView() const { return _debugView; }
        void setDebugView(const std::string& value) { _debugView = value; }

        /// For getLinearScreenDepth: whether the camera is orthographic. Its clip planes are the
        /// DOF camera planes, set whatever the effects.
        void setCameraOrthographic(const bool value) { _cameraOrthographic = value; }

        void execute() override;
        void prepareShaders() override;

    private:
        Texture* _sceneTexture = nullptr;
        float _sceneDownscale = 1.0f;
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

        ComposeColorSettings _color;

        std::vector<std::shared_ptr<CameraFrameEffect>> _effects;
        ComposeEffectsBuild _effectsBuild;
        std::string _reportedErrors;
        std::string _debugView;
        bool _cameraOrthographic = false;
    };
}
