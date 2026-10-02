// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// The post chain, drawn once through QuadRender rather than through a device
// virtual implemented separately per backend. Shader sources (MSL + GLSL) and
// the shared uniform layout live in composeShaders.h.
//
#include "renderPassCompose.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/composeShaders.h"

namespace visutwin::canvas
{
    void RenderPassCompose::prepareShaders()
    {
        if (!shader()) {
            useCachedShader("compose-quad", "composeVertex", "composeFragment", compose_shaders::COMPOSE_MSL, compose_shaders::COMPOSE_GLSL);
        }
    }

    void RenderPassCompose::execute()
    {
        const auto gd = device();
        if (!gd) {
            return;
        }

        // Normally there already: the frame graph prepares every pass's shaders first.
        prepareShaders();
        if (!shader()) {
            return;
        }

        compose_shaders::ComposeUniforms uniforms{};
        uniforms.dofEnabled = _dofEnabled ? 1u : 0u;
        uniforms.taaEnabled = _taaEnabled ? 1u : 0u;
        uniforms.ssaoEnabled = _ssaoTexture ? 1u : 0u;
        uniforms.bloomEnabled = _bloomTexture ? 1u : 0u;
        uniforms.blurTextureUpscale = _blurTextureUpscale ? 1u : 0u;
        uniforms.bloomIntensity = _bloomIntensity;
        uniforms.dofIntensity = _dofIntensity;
        // The CAS kernel is fed
        // lerp(-0.125, -0.2, sharpness): the weight must be NEGATIVE to sharpen.
        // With the raw positive user value the same kernel is a convex blend of
        // the pixel with its four neighbours, i.e. a blur, which is what this
        // pass did for as long as it existed. Zero keeps the stage off; the
        // shaders gate on `< 0`.
        uniforms.sharpness = _sharpness > 0.0f ? (-0.125f - 0.075f * _sharpness) : 0.0f;
        uniforms.tonemapMode = static_cast<uint32_t>(_toneMapping);
        uniforms.exposure = _exposure;
        if (_sceneTexture && _sceneTexture->width() > 0 && _sceneTexture->height() > 0) {
            uniforms.sceneTextureInvRes[0] = 1.0f / static_cast<float>(_sceneTexture->width());
            uniforms.sceneTextureInvRes[1] = 1.0f / static_cast<float>(_sceneTexture->height());
        }
        uniforms.sceneDownscale = _sceneDownscale;

        // Single-pass DOF (from the scene depth buffer).
        uniforms.dofFocusDistance = _dofFocusDistance;
        uniforms.dofFocusRange = _dofFocusRange;
        uniforms.dofBlurRadius = _dofBlurRadius;
        uniforms.dofCameraNear = _dofCameraNear;
        uniforms.dofCameraFar = _dofCameraFar;

        const ComposeColorSettings& c = _color;
        uniforms.vignetteEnabled = c.vignetteEnabled ? 1u : 0u;
        uniforms.vignetteInner = c.vignetteInner;
        uniforms.vignetteOuter = c.vignetteOuter;
        uniforms.vignetteCurvature = c.vignetteCurvature;
        uniforms.vignetteIntensity = c.vignetteIntensity;
        uniforms.vignetteColorR = c.vignetteColor[0];
        uniforms.vignetteColorG = c.vignetteColor[1];
        uniforms.vignetteColorB = c.vignetteColor[2];

        // The user value scaled to shader units.
        uniforms.fringingIntensity = c.fringingIntensity / 1024.0f;

        uniforms.gradingEnabled = c.gradingEnabled ? 1u : 0u;
        uniforms.gradingBrightness = c.gradingBrightness;
        uniforms.gradingContrast = c.gradingContrast;
        uniforms.gradingSaturation = c.gradingSaturation;
        uniforms.gradingTintR = c.gradingTint[0];
        uniforms.gradingTintG = c.gradingTint[1];
        uniforms.gradingTintB = c.gradingTint[2];

        uniforms.colorEnhanceEnabled =
            (c.colorEnhanceShadows != 0.0f || c.colorEnhanceHighlights != 0.0f ||
             c.colorEnhanceVibrance != 0.0f || c.colorEnhanceDehaze != 0.0f ||
             c.colorEnhanceMidtones != 0.0f) ? 1u : 0u;
        uniforms.ceShadows = c.colorEnhanceShadows;
        uniforms.ceHighlights = c.colorEnhanceHighlights;
        uniforms.ceVibrance = c.colorEnhanceVibrance;
        uniforms.ceDehaze = c.colorEnhanceDehaze;
        uniforms.ceMidtones = c.colorEnhanceMidtones;

        uniforms.lutEnabled = c.colorLUT ? 1u : 0u;
        uniforms.lut2Enabled = c.colorLUT2 ? 1u : 0u;
        uniforms.lutIntensity1 = c.colorLUTIntensity;
        uniforms.lutIntensity2 = c.colorLUTIntensity2;
        uniforms.lutBlend = c.colorLUTBlend;

        // Slots match the shader declarations in composeShaders.h. 6 and 7 carry the
        // multi-pass DOF's CoC and blur; when both are bound the shader reads them
        // instead of running its single-pass depth blur.
        const bool multipassDof = _cocTexture != nullptr && _blurTexture != nullptr;
        uniforms.dofMultipass = multipassDof ? 1u : 0u;
        if (multipassDof && _blurTexture->width() > 0 && _blurTexture->height() > 0) {
            uniforms.dofBlurTexelX = 1.0f / static_cast<float>(_blurTexture->width());
            uniforms.dofBlurTexelY = 1.0f / static_cast<float>(_blurTexture->height());
        }
        setQuadTextureBinding(0, _sceneTexture);
        setQuadTextureBinding(1, _bloomTexture);
        setQuadTextureBinding(2, _ssaoTexture);
        setQuadTextureBinding(3, _depthTexture);
        setQuadTextureBinding(4, _color.colorLUT);
        setQuadTextureBinding(5, _color.colorLUT2);
        setQuadTextureBinding(6, _cocTexture);
        setQuadTextureBinding(7, _blurTexture);
        setQuadUniforms(uniforms);
        RenderPassShaderQuad::execute();
    }
}
