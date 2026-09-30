// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Uniform packing, ring-buffer allocation, and per-pass deduplication.
// Extracted from MetalGraphicsDevice for single-responsibility decomposition.
//
#include "metalUniformBinder.h"

#include "platform/graphics/lightingDerivation.h"

#include <algorithm>
#include <cassert>
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
        // The values are decided in deriveLighting, shared with the Vulkan backend; this only
        // lays them out in LightingUniforms.
        markLightingChanged();
        auto& lu = _lightingUniforms;
        const DerivedLighting derived = deriveLighting(ambientColor, lights, std::size(lu.lights),
            fogParams, shadowParams, ambientSH, viewProjection);

        std::memcpy(lu.ambientSH, derived.ambientSH, sizeof(lu.ambientSH));
        std::memcpy(lu.viewProjection, derived.viewProjection, sizeof(lu.viewProjection));
        std::memcpy(&lu.ambientColor, derived.ambient, sizeof(derived.ambient));

        lu.lightCountAndFlags[0] = derived.lightCount;
        lu.lightCountAndFlags[1] = 0u;
        lu.lightCountAndFlags[2] = 0u;
        lu.lightCountAndFlags[3] = 0u;
        for (size_t i = 0; i < std::size(lu.lights); ++i) {
            auto& dst = lu.lights[i];
            if (i >= derived.lightCount) {
                dst = GpuLightUniform{};
                continue;
            }
            const DerivedLight& light = derived.lights[i];
            const GpuLightData& src = *light.source;
            src.position.store(&dst.positionRange.x);
            dst.positionRange[3] = src.range;
            src.direction.store(&dst.directionCone.x);
            dst.colorIntensity[0] = light.linearColor[0];
            dst.colorIntensity[1] = light.linearColor[1];
            dst.colorIntensity[2] = light.linearColor[2];
            dst.colorIntensity[3] = src.intensity;
            if (src.type == GpuLightType::AreaRect) {
                // Area rect: the cone slots carry the half-extents and the right axis.
                // directionCone[3] = areaHalfWidth, coneAngles[0] = areaHalfHeight,
                // coneAngles[1..3] = areaRight.
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
            // The light's shadow slot: a local slot (0/1), or for a directional light its
            // directional slot. Encoded as uint.
            dst.typeCastShadows[3] = (src.shadowMapIndex >= 0) ? static_cast<uint32_t>(src.shadowMapIndex) : 0u;
            dst.cookieFlags[0] = (src.cookieIndex >= 0 && src.cookie) ? 1u : 0u;
            dst.cookieFlags[1] = (src.cookieIndex >= 0) ? static_cast<uint32_t>(src.cookieIndex) : 0u;
            dst.cookieFlags[2] = src.cookieChannel;
            dst.cookieFlags[3] = src.cookieFalloff ? 1u : 0u;
        }

        if (enableNormalMaps) {
            lu.flagsAndPad[0] |= (1u << 2);
        } else {
            lu.flagsAndPad[0] &= ~(1u << 2);
        }
        cameraPosition.store(&lu.cameraPositionSkyboxIntensity.x);
        lu.skyboxMipAndPad[1] = exposure;
        // forward-fragment-tail uses this to select the tone mapping curve
        // when CameraFrame is not active (non-deferred path).
        lu.skyboxMipAndPad[2] = static_cast<float>(toneMapping);

        std::memcpy(&lu.fogColorDensity, derived.fogColorDensity, sizeof(derived.fogColorDensity));
        std::memcpy(&lu.fogStartEndType, derived.fogStartEndType, sizeof(derived.fogStartEndType));

        // Directional shadow slots. The PCSS sample counts belong to the variant,
        // so they ride slot 0's pcssParams for both slots.
        const auto packDirectional = [&](const int slot,
            PackedVector4f& biasNormalStrength, float* palette, PackedVector4f& distances,
            PackedVector4f& cascadeParams, PackedVector4f& pcssParams, PackedVector4f& pcssRadii,
            PackedVector4f& pcssDepthRanges) {
            const auto& dir = shadowParams.directional[slot];
            biasNormalStrength[0] = dir.bias;
            biasNormalStrength[1] = dir.normalBias;
            biasNormalStrength[2] = dir.strength;
            biasNormalStrength[3] = derived.directionalActive[slot] ? 1.0f : 0.0f;
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
        packDirectional(0, lu.shadowBiasNormalStrength, lu.shadowMatrixPalette, lu.shadowCascadeDistances,
            lu.shadowCascadeParams, lu.pcssParams, lu.pcssCascadeRadii, lu.pcssCascadeDepthRanges);
        packDirectional(1, lu.shadow1BiasNormalStrength, lu.shadow1MatrixPalette, lu.shadow1CascadeDistances,
            lu.shadow1CascadeParams, lu.shadow1PcssParams, lu.shadow1PcssCascadeRadii,
            lu.shadow1PcssCascadeDepthRanges);
        _shadowTexture = derived.directionalActive[0] ? shadowParams.directional[0].shadowMap : nullptr;
        _shadowTexture1 = derived.directionalActive[1] ? shadowParams.directional[1].shadowMap : nullptr;

        // Local light shadows (spot 2D, omni cube). Omni params: near, far, relative bias,
        // normal bias, with the intensity in the Extra lane.
        const DerivedLocalShadow& local0 = derived.localShadows[0];
        const DerivedLocalShadow& local1 = derived.localShadows[1];
        _localShadowTexture0 = local0.spotMap;
        _localShadowTexture1 = local1.spotMap;
        _omniShadowCube0 = local0.omniMap;
        _omniShadowCube1 = local1.omniMap;
        const auto packLocal = [](const DerivedLocalShadow& ls, float* matrix, PackedVector4f& params,
                                  PackedVector4f& pcss, PackedVector4f& omni, PackedVector4f& omniExtra) {
            std::memcpy(matrix, ls.matrix, sizeof(ls.matrix));
            std::memcpy(&params, ls.params, sizeof(ls.params));
            std::memcpy(&pcss, ls.pcss, sizeof(ls.pcss));
            if (ls.active && ls.isOmni) {
                omni[0] = ls.omniNear;
                omni[1] = ls.omniFar;
                omni[2] = ls.omniBias;
                omni[3] = ls.params[1];
                omniExtra[0] = ls.params[2];
            }
        };
        packLocal(local0, lu.localShadowMatrix0, lu.localShadowParams0, lu.localShadowPcss0,
            lu.omniShadowParams0, lu.omniShadowParams0Extra);
        packLocal(local1, lu.localShadowMatrix1, lu.localShadowParams1, lu.localShadowPcss1,
            lu.omniShadowParams1, lu.omniShadowParams1Extra);

        // Light cookies: two 2D (spot) and two cube (omni) slots.
        const auto packCookie = [](const DerivedCookieSlot& slot, Texture*& texture, float* matrix,
                                   PackedVector4f& params) {
            texture = slot.texture;
            std::memcpy(matrix, slot.matrix, sizeof(slot.matrix));
            std::memcpy(&params, slot.params, sizeof(slot.params));
        };
        packCookie(derived.cookie2D[0], _cookieTexture2D0, lu.cookieMatrix2D0, lu.cookieParams2D0);
        packCookie(derived.cookie2D[1], _cookieTexture2D1, lu.cookieMatrix2D1, lu.cookieParams2D1);
        packCookie(derived.cookieCube[0], _cookieTextureCube0, lu.cookieMatrixCube0, lu.cookieParamsCube0);
        packCookie(derived.cookieCube[1], _cookieTextureCube1, lu.cookieMatrixCube1, lu.cookieParamsCube1);
    }

    // -----------------------------------------------------------------------
    // Environment uniforms
    // -----------------------------------------------------------------------

    void MetalUniformBinder::setCameraClipPlanes(const float nearClip, const float farClip)
    {
        _lightingUniforms.cameraNearFar[0] = nearClip;
        _lightingUniforms.cameraNearFar[1] = farClip;
        markLightingChanged();
    }

    void MetalUniformBinder::setDebugShaderPass(const uint32_t mode)
    {
        // flagsAndPad[0] is the bitfield; [1] carries the debug pass mode as a plain value.
        _lightingUniforms.flagsAndPad[1] = mode;
        markLightingChanged();
    }

    void MetalUniformBinder::setReflectionProbeUniforms(Texture* cubemap, const Vector3& boxMin,
        const Vector3& boxMax, const bool boxProjection, const float intensity, const float maxLod)
    {
        _reflectionProbeCubeTexture = cubemap;
        markLightingChanged();
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
        markLightingChanged();
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

    const void* MetalUniformBinder::sharedDefaultBlockKey()
    {
        static const char key = 0;
        return &key;
    }

    void MetalUniformBinder::submitPerDrawUniforms(MTL::RenderCommandEncoder* encoder,
        MetalUniformRingBuffer* uniformRing,
        const void* materialKey,
        const void* uniformData,
        const size_t uniformSize,
        const bool hdrPass)
    {
        // Material-slot block at slot 3 — skip the ring allocation when the block is the
        // one the previous draw uploaded (consecutive draws sharing a material carry
        // identical data), or the default block a material-less draw of this pass already
        // uploaded: every opaque shadow caster and prepass draw carries that same block,
        // and each used to take a ring slot of its own.
        //
        // A null key is NOT a cache key: a quad pass has no material and puts its own
        // block in this slot, so every quad draw would compare equal to the last and
        // silently reuse the FIRST draw's uniforms. That is invisible while a pass draws
        // one quad, which every effect did until the environment bakes started drawing a
        // rect list in one pass — there the convolve draws read the reproject block and
        // produced nothing.
        size_t materialOffset;
        if (materialKey != nullptr && _materialBoundThisPass && materialKey == _lastMaterialKey) {
            materialOffset = _lastMaterialOffset;
        } else if (materialKey == sharedDefaultBlockKey() && _defaultBlockBoundThisPass) {
            materialOffset = _defaultBlockOffset;
        } else {
            materialOffset = uniformRing->allocate(uniformData, uniformSize);
            if (materialKey == sharedDefaultBlockKey()) {
                _defaultBlockOffset = materialOffset;
                _defaultBlockBoundThisPass = true;
            }
        }
        _lastMaterialKey = materialKey;
        _lastMaterialOffset = materialOffset;
        _materialBoundThisPass = true;
        // Nothing else moves slot 3's offset within a pass, so an unchanged offset is
        // already on the encoder.
        if (!_materialOffsetSetThisPass || materialOffset != _encoderMaterialOffset) {
            encoder->setFragmentBufferOffset(materialOffset, 3);
            encoder->setVertexBufferOffset(materialOffset, 3);
            _encoderMaterialOffset = materialOffset;
            _materialOffsetSetThisPass = true;
        }

        // Set HDR pass flag (bit 5) — forward shaders check this at runtime
        // to skip tonemapping + gamma when CameraFrame handles them.
        const uint32_t flags = hdrPass
            ? (_lightingUniforms.flagsAndPad[0] | (1u << 5))
            : (_lightingUniforms.flagsAndPad[0] & ~(1u << 5));
        if (flags != _lightingUniforms.flagsAndPad[0]) {
            _lightingUniforms.flagsAndPad[0] = flags;
            markLightingChanged();
        }

        // LightingUniforms at slot 4: reuse the previous upload when the block is
        // unchanged, which is nearly every draw — the renderer sets it once per layer.
        // "Unchanged" is a VERSION every writer of the block bumps, so a draw costs one
        // integer compare. It was a memcmp of the whole ~2.6 KB block per draw (and
        // before that an FNV-1a hash of it): 8-12% of the frame's CPU at 10-20k draws,
        // 17% of a shadow pass, whose draws never read lighting at all. The block is
        // still compared exactly when the version HAS moved, so a writer that put the
        // same values back (the per-layer setLightingUniforms of a second sublayer)
        // costs one compare rather than an upload.
        if (!_lightingBoundThisPass || _lightingVersion != _lastLightingVersion) {
            if (!_lightingBoundThisPass ||
                std::memcmp(&_lightingUniforms, &_lastLightingUniforms, sizeof(LightingUniforms)) != 0) {
                _lastLightingOffset = uniformRing->allocate(&_lightingUniforms, sizeof(LightingUniforms));
                std::memcpy(&_lastLightingUniforms, &_lightingUniforms, sizeof(LightingUniforms));
                // Slot 4's offset is moved only here, so it needs setting only on upload.
                encoder->setFragmentBufferOffset(_lastLightingOffset, 4);
                _lightingBoundThisPass = true;
            }
            _lastLightingVersion = _lightingVersion;
        } else {
            // A writer of _lightingUniforms that skipped markLightingChanged() shows here.
            assert(std::memcmp(&_lightingUniforms, &_lastLightingUniforms, sizeof(LightingUniforms)) == 0);
        }
    }

    // -----------------------------------------------------------------------
    // Pass lifecycle
    // -----------------------------------------------------------------------

    void MetalUniformBinder::resetPassState()
    {
        _sceneDataBoundThisPass = false;
        _lightingBoundThisPass = false;
        _materialBoundThisPass = false;
        _lastMaterialKey = nullptr;
        _defaultBlockBoundThisPass = false;
        _materialOffsetSetThisPass = false;
    }
}
