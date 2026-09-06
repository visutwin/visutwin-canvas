// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
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
    void RenderPassCompose::execute()
    {
        const auto gd = device();
        if (!gd) {
            return;
        }

        if (!shader()) {
            constexpr const char* cacheKey = "compose-quad";
            auto cached = gd->getCachedShader(cacheKey);
            if (!cached) {
                ShaderDefinition definition;
                definition.name = cacheKey;
                definition.vshader = "composeVertex";
                definition.fshader = "composeFragment";
                cached = createShader(gd.get(), definition,
                    gd->shaderLanguage() == ShaderLanguage::Glsl
                        ? compose_shaders::COMPOSE_GLSL
                        : compose_shaders::COMPOSE_MSL);
                if (cached) {
                    gd->setCachedShader(cacheKey, cached);
                }
            }
            setShader(cached);
        }
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
        // Upstream (render-pass-compose.js) feeds the CAS kernel
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

        // Single-pass DOF (from the scene depth buffer).
        uniforms.dofFocusDistance = _dofFocusDistance;
        uniforms.dofFocusRange = _dofFocusRange;
        uniforms.dofBlurRadius = _dofBlurRadius;
        uniforms.dofCameraNear = _dofCameraNear;
        uniforms.dofCameraFar = _dofCameraFar;

        uniforms.vignetteEnabled = _vignetteEnabled ? 1u : 0u;
        uniforms.vignetteInner = _vignetteInner;
        uniforms.vignetteOuter = _vignetteOuter;
        uniforms.vignetteCurvature = _vignetteCurvature;
        uniforms.vignetteIntensity = _vignetteIntensity;
        uniforms.vignetteColorR = _vignetteColor[0];
        uniforms.vignetteColorG = _vignetteColor[1];
        uniforms.vignetteColorB = _vignetteColor[2];

        uniforms.fringingIntensity = _fringingIntensity;

        uniforms.gradingEnabled = _gradingEnabled ? 1u : 0u;
        uniforms.gradingBrightness = _gradingBrightness;
        uniforms.gradingContrast = _gradingContrast;
        uniforms.gradingSaturation = _gradingSaturation;
        uniforms.gradingTintR = _gradingTint[0];
        uniforms.gradingTintG = _gradingTint[1];
        uniforms.gradingTintB = _gradingTint[2];

        uniforms.colorEnhanceEnabled =
            (_colorEnhanceShadows != 0.0f || _colorEnhanceHighlights != 0.0f ||
             _colorEnhanceVibrance != 0.0f || _colorEnhanceDehaze != 0.0f ||
             _colorEnhanceMidtones != 0.0f) ? 1u : 0u;
        uniforms.ceShadows = _colorEnhanceShadows;
        uniforms.ceHighlights = _colorEnhanceHighlights;
        uniforms.ceVibrance = _colorEnhanceVibrance;
        uniforms.ceDehaze = _colorEnhanceDehaze;
        uniforms.ceMidtones = _colorEnhanceMidtones;

        uniforms.lutEnabled = _colorLUT ? 1u : 0u;
        uniforms.lut2Enabled = _colorLUT2 ? 1u : 0u;
        uniforms.lutIntensity1 = _colorLUTIntensity;
        uniforms.lutIntensity2 = _colorLUTIntensity2;
        uniforms.lutBlend = _colorLUTBlend;

        // Slots match the shader declarations in composeShaders.h. The old device
        // path also bound _cocTexture and _blurTexture, but the multi-pass DOF
        // branch that read them has been commented out for a long time — only
        // applyDofSinglePass runs — so those two bindings are gone.
        setQuadTextureBinding(0, _sceneTexture);
        setQuadTextureBinding(1, _bloomTexture);
        setQuadTextureBinding(2, _ssaoTexture);
        setQuadTextureBinding(3, _depthTexture);
        setQuadTextureBinding(4, _colorLUT);
        setQuadTextureBinding(5, _colorLUT2);
        setQuadUniforms(uniforms);
        RenderPassShaderQuad::execute();
    }
}
