// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Uniform packing, ring-buffer allocation, and per-pass deduplication.
// Extracted from MetalGraphicsDevice for single-responsibility decomposition.
//
#include "metalUniformBinder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "metalUniformRingBuffer.h"
#include "metalUtils.h"
#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "core/utils.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/material.h"

namespace visutwin::canvas
{
    using metal::toSimdMatrix;

    namespace
    {
        struct SceneData
        {
            simd::float4x4 projViewMatrix;
        };

        struct ModelData
        {
            simd::float4x4 modelMatrix;
            simd::float4x4 normalMatrix;
            float normalSign;
            float _pad[3];
        };
    }

    // -----------------------------------------------------------------------
    // Transform uniforms
    // -----------------------------------------------------------------------

    void MetalUniformBinder::setTransformUniforms(MTL::RenderCommandEncoder* encoder,
        MetalUniformRingBuffer* transformRing,
        const Matrix4& viewProjection, const Matrix4& model)
    {
        if (!encoder) {
            return;
        }

        // SceneData (VP matrix) at slot 1 — only re-send when the VP changes.
        // Within a single camera/layer pass the VP is identical for all draws,
        // but multi-layer passes may switch cameras with different VP matrices.
        const SceneData sceneData{toSimdMatrix(viewProjection)};
        if (!_sceneDataBoundThisPass ||
            std::memcmp(&sceneData.projViewMatrix, &_cachedSceneVP, sizeof(simd::float4x4)) != 0) {
            encoder->setVertexBytes(&sceneData, sizeof(SceneData), 1);
            _cachedSceneVP = sceneData.projViewMatrix;
            _sceneDataBoundThisPass = true;
        }

        // ModelData at slot 2 — changes per draw, allocated from ring buffer.
        // Normal matrix = (M^-1)^T of the upper-left 3x3.
        // Computed via 3x3 cofactors instead of full 4x4 inverse().transpose() (~5-7x cheaper).
        // The cofactor matrix C satisfies: (M^-1)^T = C / det(M).
        // Since the vertex shader multiplies by float4(normal, 0.0), only the 3x3 matters.
        const float m00 = model.getElement(0, 0);
        const float m01 = model.getElement(0, 1);
        const float m02 = model.getElement(0, 2);
        const float m10 = model.getElement(1, 0);
        const float m11 = model.getElement(1, 1);
        const float m12 = model.getElement(1, 2);
        const float m20 = model.getElement(2, 0);
        const float m21 = model.getElement(2, 1);
        const float m22 = model.getElement(2, 2);

        // 3x3 cofactors (reused for both determinant and normal matrix).
        const float c00 = m11 * m22 - m12 * m21;
        const float c01 = m12 * m20 - m10 * m22;
        const float c02 = m10 * m21 - m11 * m20;
        const float c10 = m02 * m21 - m01 * m22;
        const float c11 = m00 * m22 - m02 * m20;
        const float c12 = m01 * m20 - m00 * m21;
        const float c20 = m01 * m12 - m02 * m11;
        const float c21 = m02 * m10 - m00 * m12;
        const float c22 = m00 * m11 - m01 * m10;

        const float det3 = m00 * c00 + m01 * c01 + m02 * c02;
        const float normalSign = det3 < 0.0f ? -1.0f : 1.0f;
        const float invDet = (std::abs(det3) > 1e-8f) ? (1.0f / det3) : 0.0f;

        // Pack cofactor/det into simd::float4x4 directly (avoids Matrix4 intermediary).
        const simd::float4x4 normalMatrix = simd::float4x4(
            simd::float4{c00 * invDet, c01 * invDet, c02 * invDet, 0.0f},
            simd::float4{c10 * invDet, c11 * invDet, c12 * invDet, 0.0f},
            simd::float4{c20 * invDet, c21 * invDet, c22 * invDet, 0.0f},
            simd::float4{0.0f, 0.0f, 0.0f, 1.0f}
        );

        const ModelData modelData{
            toSimdMatrix(model),
            normalMatrix,
            normalSign,
            {0.0f, 0.0f, 0.0f}
        };
        // Allocate ModelData from ring buffer and update offset (slot 2).
        // The ring buffer itself was bound to slot 2 in startRenderPass().
        const size_t transformOffset = transformRing->allocate(&modelData, sizeof(ModelData));
        encoder->setVertexBufferOffset(transformOffset, 2);
    }

    // -----------------------------------------------------------------------
    // Lighting uniforms
    // -----------------------------------------------------------------------

    void MetalUniformBinder::setLightingUniforms(const Color& ambientColor,
        const std::vector<GpuLightData>& lights, const Vector3& cameraPosition,
        const bool enableNormalMaps, const float exposure,
        const FogParams& fogParams, const ShadowParams& shadowParams,
        const int toneMapping)
    {
        // scene ambient color is authored in sRGB and converted to linear.
        Color ambientLinear;
        ambientLinear.linear(&ambientColor);
        _lightingUniforms.ambientColor[0] = ambientLinear.r;
        _lightingUniforms.ambientColor[1] = ambientLinear.g;
        _lightingUniforms.ambientColor[2] = ambientLinear.b;
        _lightingUniforms.ambientColor[3] = 0.0f;

        const size_t maxLights = std::size(_lightingUniforms.lights);
        const size_t lightCount = std::min(lights.size(), maxLights);
        _lightingUniforms.lightCountAndFlags[0] = static_cast<uint32_t>(lightCount);
        _lightingUniforms.lightCountAndFlags[1] = 0u;
        _lightingUniforms.lightCountAndFlags[2] = 0u;
        _lightingUniforms.lightCountAndFlags[3] = 0u;

        for (size_t i = 0; i < maxLights; ++i) {
            auto& dst = _lightingUniforms.lights[i];
            if (i < lightCount) {
                const auto& src = lights[i];
                Color lightLinear;
                lightLinear.linear(&src.color);
                dst.positionRange[0] = src.position.getX();
                dst.positionRange[1] = src.position.getY();
                dst.positionRange[2] = src.position.getZ();
                dst.positionRange[3] = src.range;
                dst.directionCone[0] = src.direction.getX();
                dst.directionCone[1] = src.direction.getY();
                dst.directionCone[2] = src.direction.getZ();
                dst.colorIntensity[0] = lightLinear.r;
                dst.colorIntensity[1] = lightLinear.g;
                dst.colorIntensity[2] = lightLinear.b;
                dst.colorIntensity[3] = src.intensity;
                if (src.type == GpuLightType::AreaRect) {
                    // Area rect: repurpose cone slots for half-extents + right axis.
                    // directionCone[3] = areaHalfWidth  (was outerConeCos)
                    // coneAngles[0]    = areaHalfHeight  (was innerConeCos)
                    // coneAngles[1..3] = areaRight.xyz   (was outerConeCos/pad/pad)
                    dst.directionCone[3] = src.areaHalfWidth;
                    dst.coneAngles[0] = src.areaHalfHeight;
                    dst.coneAngles[1] = src.areaRight.getX();
                    dst.coneAngles[2] = src.areaRight.getY();
                    dst.coneAngles[3] = src.areaRight.getZ();
                } else {
                    dst.directionCone[3] = src.outerConeCos;
                    dst.coneAngles[0] = src.innerConeCos;
                    dst.coneAngles[1] = src.outerConeCos;
                    dst.coneAngles[2] = 0.0f;
                    dst.coneAngles[3] = 0.0f;
                }
                dst.typeCastShadows[0] = static_cast<uint32_t>(src.type);
                dst.typeCastShadows[1] = src.castShadows ? 1u : 0u;
                dst.typeCastShadows[2] = src.falloffModeLinear ? 1u : 0u;
                // Local shadow map index: 0 or 1 → texture slots 11/12. Encoded as uint.
                dst.typeCastShadows[3] = (src.shadowMapIndex >= 0)
                    ? static_cast<uint32_t>(src.shadowMapIndex) : 0u;
            } else {
                dst = GpuLightUniform{};
            }
        }

        if (enableNormalMaps) {
            _lightingUniforms.flagsAndPad[0] |= (1u << 2);
        } else {
            _lightingUniforms.flagsAndPad[0] &= ~(1u << 2);
        }
        _lightingUniforms.cameraPositionSkyboxIntensity[0] = cameraPosition.getX();
        _lightingUniforms.cameraPositionSkyboxIntensity[1] = cameraPosition.getY();
        _lightingUniforms.cameraPositionSkyboxIntensity[2] = cameraPosition.getZ();
        _lightingUniforms.skyboxMipAndPad[1] = exposure;
        // forward-fragment-tail uses this to select the tone mapping curve
        // when CameraFrame is not active (non-deferred path).
        _lightingUniforms.skyboxMipAndPad[2] = static_cast<float>(toneMapping);

        Color fogLinear;
        fogLinear.linear(&fogParams.color);
        _lightingUniforms.fogColorDensity[0] = fogLinear.r;
        _lightingUniforms.fogColorDensity[1] = fogLinear.g;
        _lightingUniforms.fogColorDensity[2] = fogLinear.b;
        _lightingUniforms.fogColorDensity[3] = fogParams.density;
        _lightingUniforms.fogStartEndType[0] = fogParams.start;
        _lightingUniforms.fogStartEndType[1] = fogParams.end;
        _lightingUniforms.fogStartEndType[2] = fogParams.enabled ? 1.0f : 0.0f;
        _lightingUniforms.fogStartEndType[3] = 0.0f;

        _lightingUniforms.shadowBiasNormalStrength[0] = shadowParams.bias;
        _lightingUniforms.shadowBiasNormalStrength[1] = shadowParams.normalBias;
        _lightingUniforms.shadowBiasNormalStrength[2] = shadowParams.strength;
        _lightingUniforms.shadowBiasNormalStrength[3] = shadowParams.enabled ? 1.0f : 0.0f;

        // CSM: pack cascade matrix palette, distances, and params.
        //lines 279-282.
        std::memcpy(_lightingUniforms.shadowMatrixPalette,
                    shadowParams.shadowMatrixPalette,
                    sizeof(_lightingUniforms.shadowMatrixPalette));
        std::memcpy(_lightingUniforms.shadowCascadeDistances,
                    shadowParams.shadowCascadeDistances,
                    sizeof(_lightingUniforms.shadowCascadeDistances));
        _lightingUniforms.shadowCascadeParams[0] = static_cast<float>(shadowParams.numCascades);
        _lightingUniforms.shadowCascadeParams[1] = shadowParams.cascadeBlend;
        _lightingUniforms.shadowCascadeParams[2] = 0.0f;
        _lightingUniforms.shadowCascadeParams[3] = 0.0f;

        _shadowTexture = shadowParams.shadowMap;

        // Local light shadows (spot/point): pack VP matrices and per-light params.
        // Omni lights use cubemap shadow textures (bound separately); spot lights use 2D.
        _localShadowTexture0 = nullptr;
        _localShadowTexture1 = nullptr;
        _omniShadowCube0 = nullptr;
        _omniShadowCube1 = nullptr;

        // Helper lambda to pack a local shadow entry.
        auto packLocalShadow = [&](int idx, const ShadowParams::LocalShadow& ls) {
            float* matDst = (idx == 0) ? _lightingUniforms.localShadowMatrix0 : _lightingUniforms.localShadowMatrix1;
            float* paramsDst = (idx == 0) ? _lightingUniforms.localShadowParams0 : _lightingUniforms.localShadowParams1;

            if (ls.isOmni) {
                // Omni: bind cubemap texture, pack omni-specific params.
                // The shader-side depth bias must be very small because perspective
                // depth is highly compressed near 1.0 for cubemap shadow maps.
                // The primary self-shadowing prevention is hardware polygon offset
                // (setDepthBias in the shadow render pass), so the shader bias is
                // only a secondary guard.  Use a fixed small value (0.001) rather
                // than the light's shadowBias which is meant for polygon offset
                // (scaled by ×1000 in the render pass).
                constexpr float omniShaderBias = 0.001f;
                const float farClip = ls.viewProjection.getElement(0, 0);
                if (idx == 0) {
                    _omniShadowCube0 = ls.shadowMap;
                    _lightingUniforms.omniShadowParams0[0] = 0.01f;  // near
                    _lightingUniforms.omniShadowParams0[1] = farClip;  // far (stored in VP[0][0] by renderer)
                    _lightingUniforms.omniShadowParams0[2] = omniShaderBias;
                    _lightingUniforms.omniShadowParams0[3] = ls.normalBias;
                    _lightingUniforms.omniShadowParams0Extra[0] = ls.intensity;
                } else {
                    _omniShadowCube1 = ls.shadowMap;
                    _lightingUniforms.omniShadowParams1[0] = 0.01f;
                    _lightingUniforms.omniShadowParams1[1] = farClip;
                    _lightingUniforms.omniShadowParams1[2] = omniShaderBias;
                    _lightingUniforms.omniShadowParams1[3] = ls.normalBias;
                    _lightingUniforms.omniShadowParams1Extra[0] = ls.intensity;
                }
                // Don't set 2D shadow texture for omni lights.
                std::memset(matDst, 0, 16 * sizeof(float));
            } else {
                // Spot: bind 2D texture, pack VP matrix.
                if (idx == 0) {
                    _localShadowTexture0 = ls.shadowMap;
                } else {
                    _localShadowTexture1 = ls.shadowMap;
                }
                const auto& m = ls.viewProjection;
                for (int col = 0; col < 4; ++col) {
                    for (int row = 0; row < 4; ++row) {
                        matDst[col * 4 + row] = m.getElement(row, col);
                    }
                }
            }
            paramsDst[0] = ls.bias;
            paramsDst[1] = ls.normalBias;
            paramsDst[2] = ls.intensity;
            paramsDst[3] = ls.isOmni ? 1.0f : 0.0f;  // Flag: 1=omni cubemap, 0=spot 2D
        };

        for (int i = 0; i < ShadowParams::kMaxLocalShadows; ++i) {
            if (i < shadowParams.localShadowCount) {
                packLocalShadow(i, shadowParams.localShadows[i]);
            } else {
                // Clear unused slots.
                float* matDst = (i == 0) ? _lightingUniforms.localShadowMatrix0 : _lightingUniforms.localShadowMatrix1;
                float* paramsDst = (i == 0) ? _lightingUniforms.localShadowParams0 : _lightingUniforms.localShadowParams1;
                std::memset(matDst, 0, 16 * sizeof(float));
                paramsDst[0] = 0.0001f;
                paramsDst[1] = 0.0f;
                paramsDst[2] = 1.0f;
                paramsDst[3] = 0.0f;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Environment uniforms
    // -----------------------------------------------------------------------

    void MetalUniformBinder::setEnvironmentUniforms(Texture* envAtlas, const float skyboxIntensity,
        const float skyboxMip, const Vector3& skyDomeCenter, const bool isDome,
        Texture* skyboxCubeMap)
    {
        _envAtlasTexture = envAtlas;
        _skyboxCubeMapTexture = skyboxCubeMap;
        _lightingUniforms.cameraPositionSkyboxIntensity[3] = skyboxIntensity;
        _lightingUniforms.skyboxMipAndPad[0] = skyboxMip;

        // pack dome center for SKYTYPE_DOME/BOX
        _lightingUniforms.skyDomeCenter[0] = skyDomeCenter.getX();
        _lightingUniforms.skyDomeCenter[1] = skyDomeCenter.getY();
        _lightingUniforms.skyDomeCenter[2] = skyDomeCenter.getZ();
        _lightingUniforms.skyDomeCenter[3] = isDome ? 1.0f : 0.0f;
        if (_envAtlasTexture) {
            _lightingUniforms.flagsAndPad[0] |= (1u << 1);
            if (_envAtlasTexture->encoding() == TextureEncoding::RGBP) {
                _lightingUniforms.flagsAndPad[0] |= (1u << 3);
            } else {
                _lightingUniforms.flagsAndPad[0] &= ~(1u << 3);
            }
            if (_envAtlasTexture->encoding() == TextureEncoding::RGBM) {
                _lightingUniforms.flagsAndPad[0] |= (1u << 4);
            } else {
                _lightingUniforms.flagsAndPad[0] &= ~(1u << 4);
            }
        } else {
            _lightingUniforms.flagsAndPad[0] &= ~(1u << 1);
            _lightingUniforms.flagsAndPad[0] &= ~(1u << 3);
            _lightingUniforms.flagsAndPad[0] &= ~(1u << 4);
        }
    }

    // -----------------------------------------------------------------------
    // Per-draw uniform submission with deduplication
    // -----------------------------------------------------------------------

    void MetalUniformBinder::submitPerDrawUniforms(MTL::RenderCommandEncoder* encoder,
        MetalUniformRingBuffer* uniformRing,
        const Material* currentMaterial,
        const void* uniformData,
        const size_t uniformSize,
        const bool hdrPass)
    {
        // Material uniforms at slot 3 — skip ring allocation when same material is
        // bound as previous draw (consecutive draws sharing a material produce
        // identical uniform data, so we can reuse the previous ring offset).
        size_t materialOffset;
        if (_materialBoundThisPass && currentMaterial == _lastBoundMaterial) {
            materialOffset = _lastMaterialOffset;
        } else {
            materialOffset = uniformRing->allocate(uniformData, uniformSize);
            _lastBoundMaterial = currentMaterial;
            _lastMaterialOffset = materialOffset;
            _materialBoundThisPass = true;
        }
        encoder->setFragmentBufferOffset(materialOffset, 3);
        encoder->setVertexBufferOffset(materialOffset, 3);

        // Set HDR pass flag (bit 5) — forward shaders check this at runtime
        // to skip tonemapping + gamma when CameraFrame handles them.
        if (hdrPass) {
            _lightingUniforms.flagsAndPad[0] |= (1u << 5);
        } else {
            _lightingUniforms.flagsAndPad[0] &= ~(1u << 5);
        }

        // LightingUniforms at slot 4 — hash-based deduplication to skip ring
        // allocation when lighting data hasn't changed. 95%+ of draws within a
        // layer have identical lighting because all mesh instances default to
        // MASK_AFFECT_DYNAMIC = 1 and all lights use the same default mask.
        const uint32_t lightingHash = hash32Fnv1a(
            reinterpret_cast<const uint32_t*>(&_lightingUniforms),
            sizeof(LightingUniforms) / sizeof(uint32_t));
        size_t lightingOffset;
        if (_lightingBoundThisPass && lightingHash == _lastLightingHash) {
            lightingOffset = _lastLightingOffset;
        } else {
            lightingOffset = uniformRing->allocate(&_lightingUniforms, sizeof(LightingUniforms));
            _lastLightingHash = lightingHash;
            _lastLightingOffset = lightingOffset;
            _lightingBoundThisPass = true;
        }
        encoder->setFragmentBufferOffset(lightingOffset, 4);
    }

    // -----------------------------------------------------------------------
    // Pass lifecycle
    // -----------------------------------------------------------------------

    void MetalUniformBinder::resetPassState()
    {
        _sceneDataBoundThisPass = false;
        _lightingBoundThisPass = false;
        _materialBoundThisPass = false;
        _lastBoundMaterial = nullptr;
    }
}
