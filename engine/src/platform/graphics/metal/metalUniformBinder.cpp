// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Uniform packing, ring-buffer allocation, and per-pass deduplication.
// Extracted from MetalGraphicsDevice for single-responsibility decomposition.
//
#include "metalUniformBinder.h"

#include <algorithm>
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
        // Normal matrix = (M^-1)^T of the upper-left 3x3, built by Matrix4::normalMatrix
        // from 3x3 cofactors instead of a full 4x4 inverse().transpose() (~5-7x cheaper).
        // The cofactor matrix C satisfies: (M^-1)^T = C / det(M).
        // Since the vertex shader multiplies by float4(normal, 0.0), only the 3x3 matters.
        //
        // Dividing by the SIGNED determinant is what makes this the inverse
        // transpose rather than a cofactor matrix, and a mirrored mesh depends on
        // that sign: its normals flip with its surface, as upstream's matrix_normal
        // does, and the renderer flips which of its faces is culled to match.
        // A near-singular 3x3 (|det| <= 1e-8) uploads a zero normal matrix.
        const ModelData modelData{
            toSimdMatrix(model),
            toSimdMatrix(model.normalMatrix())
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
        const int toneMapping, const Vector3* ambientSH, const Matrix4* viewProjection)
    {
        // Ambient SH light probes (VT_FEATURE_LIGHT_PROBES): 9 premultiplied
        // irradiance coefficients, or zeros when disabled.
        if (ambientSH) {
            for (int i = 0; i < 9; ++i) {
                ambientSH[i].store(&_lightingUniforms.ambientSH[i].x);
                _lightingUniforms.ambientSH[i][3] = 0.0f;
            }
        } else {
            std::memset(_lightingUniforms.ambientSH, 0, sizeof(_lightingUniforms.ambientSH));
        }

        // Camera view-projection for fragment-stage screen projection
        // (dynamic grab-pass refraction). Identity when not provided.
        // Column-major, as Matrix4::store writes it: this used to pass getElement(row, col),
        // uploading the TRANSPOSE, which gives every refracting fragment a negative w and
        // clamps its grab UV into a corner - a flat, dark, opaque-looking surface.
        if (viewProjection) {
            viewProjection->store(_lightingUniforms.viewProjection);
        } else {
            std::memset(_lightingUniforms.viewProjection, 0, sizeof(_lightingUniforms.viewProjection));
            _lightingUniforms.viewProjection[0] = _lightingUniforms.viewProjection[5] =
                _lightingUniforms.viewProjection[10] = _lightingUniforms.viewProjection[15] = 1.0f;
        }

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
                src.position.store(&dst.positionRange.x);
                dst.positionRange[3] = src.range;
                src.direction.store(&dst.directionCone.x);
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
                    src.areaRight.store(&dst.coneAngles[1]);
                } else {
                    dst.directionCone[3] = src.outerConeCos;
                    dst.coneAngles[0] = src.innerConeCos;
                    dst.coneAngles[1] = src.outerConeCos;
                    dst.coneAngles[2] = 0.0f;
                    dst.coneAngles[3] = 0.0f;
                }
                dst.typeCastShadows[0] = static_cast<uint32_t>(src.type);
                // Area lights never cast shadows in this port — their castShadows
                // slot carries the shape instead (0=rect, 1=disk, 2=sphere).
                dst.typeCastShadows[1] = (src.type == GpuLightType::AreaRect)
                    ? src.areaShape : (src.castShadows ? 1u : 0u);
                dst.typeCastShadows[2] = src.falloffModeLinear ? 1u : 0u;
                // Local shadow map index: 0 or 1 → texture slots 11/12. Encoded as uint.
                dst.typeCastShadows[3] = (src.shadowMapIndex >= 0)
                    ? static_cast<uint32_t>(src.shadowMapIndex) : 0u;
                dst.cookieFlags[0] = (src.cookieIndex >= 0 && src.cookie) ? 1u : 0u;
                dst.cookieFlags[1] = (src.cookieIndex >= 0)
                    ? static_cast<uint32_t>(src.cookieIndex) : 0u;
                dst.cookieFlags[2] = src.cookieChannel;
                dst.cookieFlags[3] = src.cookieFalloff ? 1u : 0u;
            } else {
                dst = GpuLightUniform{};
            }
        }

        if (enableNormalMaps) {
            _lightingUniforms.flagsAndPad[0] |= (1u << 2);
        } else {
            _lightingUniforms.flagsAndPad[0] &= ~(1u << 2);
        }
        cameraPosition.store(&_lightingUniforms.cameraPositionSkyboxIntensity.x);
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
        _lightingUniforms.fogStartEndType[2] = fogParams.enabled
            ? static_cast<float>(fogParams.type) : 0.0f;   // 0 = off, else FogType
        _lightingUniforms.fogStartEndType[3] = 0.0f;

        // Directional shadow slots. The PCSS sample counts belong to the variant,
        // so they ride slot 0's pcssParams for both slots.
        const auto packDirectional = [&](const ShadowParams::DirectionalShadow& dir, const bool active,
            PackedVector4f& biasNormalStrength, float* palette, PackedVector4f& distances,
            PackedVector4f& cascadeParams, PackedVector4f& pcssParams, PackedVector4f& pcssRadii,
            PackedVector4f& pcssDepthRanges) {
            biasNormalStrength[0] = dir.bias;
            biasNormalStrength[1] = dir.normalBias;
            biasNormalStrength[2] = dir.strength;
            biasNormalStrength[3] = active ? 1.0f : 0.0f;
            pcssParams[0] = static_cast<float>(shadowParams.pcssSamples);
            pcssParams[1] = static_cast<float>(shadowParams.pcssBlockerSamples);
            pcssParams[2] = dir.penumbraSize;
            pcssParams[3] = dir.penumbraFalloff;
            std::memcpy(&pcssRadii, dir.pcssCascadeRadii, sizeof(pcssRadii));
            std::memcpy(&pcssDepthRanges, dir.pcssCascadeDepthRanges, sizeof(pcssDepthRanges));
            std::memcpy(palette, dir.shadowMatrixPalette, sizeof(dir.shadowMatrixPalette));
            std::memcpy(&distances, dir.shadowCascadeDistances, sizeof(distances));
            cascadeParams[0] = static_cast<float>(dir.numCascades);
            cascadeParams[1] = dir.cascadeBlend;
            cascadeParams[2] = 0.0f;
            cascadeParams[3] = 0.0f;
        };
        auto& lu = _lightingUniforms;
        packDirectional(shadowParams.directional[0], shadowParams.directionalCount > 0,
            lu.shadowBiasNormalStrength, lu.shadowMatrixPalette, lu.shadowCascadeDistances,
            lu.shadowCascadeParams, lu.pcssParams, lu.pcssCascadeRadii, lu.pcssCascadeDepthRanges);
        packDirectional(shadowParams.directional[1], shadowParams.directionalCount > 1,
            lu.shadow1BiasNormalStrength, lu.shadow1MatrixPalette, lu.shadow1CascadeDistances,
            lu.shadow1CascadeParams, lu.shadow1PcssParams, lu.shadow1PcssCascadeRadii,
            lu.shadow1PcssCascadeDepthRanges);

        _shadowTexture = shadowParams.directionalCount > 0 ? shadowParams.directional[0].shadowMap : nullptr;
        _shadowTexture1 = shadowParams.directionalCount > 1 ? shadowParams.directional[1].shadowMap : nullptr;

        // Local light shadows (spot/point): pack VP matrices and per-light params.
        // Omni lights use cubemap shadow textures (bound separately); spot lights use 2D.
        _localShadowTexture0 = nullptr;
        _localShadowTexture1 = nullptr;
        _omniShadowCube0 = nullptr;
        _omniShadowCube1 = nullptr;

        // Helper lambda to pack a local shadow entry.
        auto packLocalShadow = [&](int idx, const ShadowParams::LocalShadow& ls) {
            float* matDst = (idx == 0) ? _lightingUniforms.localShadowMatrix0 : _lightingUniforms.localShadowMatrix1;
            float* paramsDst = (idx == 0) ? &_lightingUniforms.localShadowParams0.x : &_lightingUniforms.localShadowParams1.x;

            if (ls.isOmni) {
                // Omni: bind cubemap texture, pack omni-specific params.
                // RELATIVE bias — a fraction of the receiver distance, applied by the
                // shader BEFORE the perspective projection. Perspective depth for a
                // cubemap shadow is crushed against 1.0 (with near 0.01 / far 30,
                // half a world unit of separation is 8e-5 of stored depth), so the
                // old fixed post-projection offset of 0.001 was more than ten times
                // the gap it was meant to preserve and erased omni shadows outright.
                constexpr float omniShaderBias = 0.002f;
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
                ls.viewProjection.store(matDst);  // column-major
            }
            paramsDst[0] = ls.bias;
            paramsDst[1] = ls.normalBias;
            paramsDst[2] = ls.intensity;
            paramsDst[3] = ls.isOmni ? 1.0f : 0.0f;  // Flag: 1=omni cubemap, 0=spot 2D

            float* pcssDst = (idx == 0) ? &_lightingUniforms.localShadowPcss0.x : &_lightingUniforms.localShadowPcss1.x;
            pcssDst[0] = ls.pcssSearchArea;
            pcssDst[1] = ls.nearClip;
            pcssDst[2] = ls.farClip;
            pcssDst[3] = 0.0f;
        };

        for (int i = 0; i < ShadowParams::kMaxLocalShadows; ++i) {
            if (i < shadowParams.localShadowCount) {
                packLocalShadow(i, shadowParams.localShadows[i]);
            } else {
                // Clear unused slots.
                float* matDst = (i == 0) ? _lightingUniforms.localShadowMatrix0 : _lightingUniforms.localShadowMatrix1;
                float* paramsDst = (i == 0) ? &_lightingUniforms.localShadowParams0.x : &_lightingUniforms.localShadowParams1.x;
                std::memset(matDst, 0, 16 * sizeof(float));
                paramsDst[0] = 0.0001f;
                paramsDst[1] = 0.0f;
                paramsDst[2] = 1.0f;
                paramsDst[3] = 0.0f;
                float* pcssDst = (i == 0) ? &_lightingUniforms.localShadowPcss0.x : &_lightingUniforms.localShadowPcss1.x;
                pcssDst[0] = 0.0f;
            }
        }

        // Light cookies: two 2D (spot) and two cubemap (omni) slots, indexed by
        // GpuLightData::cookieIndex within the pool the light type selects.
        _cookieTexture2D0 = nullptr;
        _cookieTexture2D1 = nullptr;
        _cookieTextureCube0 = nullptr;
        _cookieTextureCube1 = nullptr;

        auto clearCookieSlot = [](float* matDst, float* paramsDst) {
            std::memset(matDst, 0, 16 * sizeof(float));
            matDst[0] = matDst[5] = matDst[10] = matDst[15] = 1.0f;
            paramsDst[0] = 1.0f;
            paramsDst[1] = 1.0f;
            paramsDst[2] = 0.0f;
            paramsDst[3] = 0.0f;
        };
        clearCookieSlot(_lightingUniforms.cookieMatrix2D0, &_lightingUniforms.cookieParams2D0.x);
        clearCookieSlot(_lightingUniforms.cookieMatrix2D1, &_lightingUniforms.cookieParams2D1.x);
        clearCookieSlot(_lightingUniforms.cookieMatrixCube0, &_lightingUniforms.cookieParamsCube0.x);
        clearCookieSlot(_lightingUniforms.cookieMatrixCube1, &_lightingUniforms.cookieParamsCube1.x);

        for (size_t i = 0; i < lightCount; ++i) {
            const auto& src = lights[i];
            if (!src.cookie || src.cookieIndex < 0 || src.cookieIndex > 1) {
                continue;
            }
            const bool isCube = (src.type == GpuLightType::Point);
            float* matDst;
            float* paramsDst;
            if (isCube) {
                (src.cookieIndex == 0 ? _cookieTextureCube0 : _cookieTextureCube1) = src.cookie;
                matDst = (src.cookieIndex == 0)
                    ? _lightingUniforms.cookieMatrixCube0 : _lightingUniforms.cookieMatrixCube1;
                paramsDst = (src.cookieIndex == 0)
                    ? &_lightingUniforms.cookieParamsCube0.x : &_lightingUniforms.cookieParamsCube1.x;
            } else {
                (src.cookieIndex == 0 ? _cookieTexture2D0 : _cookieTexture2D1) = src.cookie;
                matDst = (src.cookieIndex == 0)
                    ? _lightingUniforms.cookieMatrix2D0 : _lightingUniforms.cookieMatrix2D1;
                paramsDst = (src.cookieIndex == 0)
                    ? &_lightingUniforms.cookieParams2D0.x : &_lightingUniforms.cookieParams2D1.x;
            }
            src.cookieMatrix.store(matDst);  // column-major
            paramsDst[0] = src.cookieIntensity;
            paramsDst[1] = src.cookieFalloff ? 1.0f : 0.0f;
            paramsDst[2] = static_cast<float>(src.cookieChannel);
            paramsDst[3] = 0.0f;
        }
    }

    // -----------------------------------------------------------------------
    // Environment uniforms
    // -----------------------------------------------------------------------

    void MetalUniformBinder::setCameraClipPlanes(const float nearClip, const float farClip)
    {
        _lightingUniforms.cameraNearFar[0] = nearClip;
        _lightingUniforms.cameraNearFar[1] = farClip;
    }

    void MetalUniformBinder::setDebugShaderPass(const uint32_t mode)
    {
        // flagsAndPad[0] is the bitfield; [1] carries the debug pass mode as a plain value.
        _lightingUniforms.flagsAndPad[1] = mode;
    }

    void MetalUniformBinder::setReflectionProbeUniforms(Texture* cubemap, const Vector3& boxMin,
        const Vector3& boxMax, const bool boxProjection, const float intensity, const float maxLod)
    {
        _reflectionProbeCubeTexture = cubemap;
        boxMin.store(&_lightingUniforms.reflectionProbeBoxMin.x);
        boxMax.store(&_lightingUniforms.reflectionProbeBoxMax.x);
        _lightingUniforms.reflectionProbeParams[0] = boxProjection ? 1.0f : 0.0f;
        _lightingUniforms.reflectionProbeParams[1] = intensity;
        _lightingUniforms.reflectionProbeParams[2] = maxLod;
    }

    void MetalUniformBinder::setEnvironmentUniforms(Texture* envAtlas, const float skyboxIntensity,
        const float skyboxMip, const Vector3& skyDomeCenter, const bool isDome,
        Texture* skyboxCubeMap)
    {
        _envAtlasTexture = envAtlas;
        _skyboxCubeMapTexture = skyboxCubeMap;
        _lightingUniforms.cameraPositionSkyboxIntensity[3] = skyboxIntensity;
        _lightingUniforms.skyboxMipAndPad[0] = skyboxMip;

        // pack dome center for SKYTYPE_DOME/BOX
        skyDomeCenter.store(&_lightingUniforms.skyDomeCenter.x);
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
    // Atmosphere uniforms
    // -----------------------------------------------------------------------

    void MetalUniformBinder::setAtmosphereUniforms(const void* data, const size_t size)
    {
        if (data && size <= sizeof(_atmosphereUniforms)) {
            std::memcpy(&_atmosphereUniforms, data, size);
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
        // Material uniforms at slot 3 — skip ring allocation when the same material
        // is bound as the previous draw (consecutive draws sharing a material
        // produce identical uniform data, so the previous ring offset is reusable).
        //
        // A null material is NOT a cache key: a quad pass has no material and puts
        // its own block in this slot, so every quad draw would compare equal to the
        // last and silently reuse the FIRST draw's uniforms. That is invisible while
        // a pass draws one quad, which every effect did until the environment bakes
        // started drawing a rect list in one pass — there the convolve draws read
        // the reproject block and produced nothing.
        size_t materialOffset;
        if (currentMaterial != nullptr && _materialBoundThisPass &&
            currentMaterial == _lastBoundMaterial) {
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
