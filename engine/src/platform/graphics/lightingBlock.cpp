// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
#include "lightingBlock.h"

#include <cstring>
#include <iterator>

#include "core/math/vector3.h"
#include "platform/graphics/lightingDerivation.h"

namespace visutwin::canvas
{
    void packLightingBlock(LightingBlock& block, LightingBlockTextures& textures, const DerivedLighting& derived,
        const ShadowParams& shadowParams, const Vector3& cameraPosition, const float exposure,
        const int toneMapping, const bool enableNormalMaps)
    {
        block.clusterParams2[1] = derived.clusterLightAccept;

        std::memcpy(block.ambientSH, derived.ambientSH, sizeof(block.ambientSH));
        // SSR and refraction project world positions to screen UV with this.
        std::memcpy(block.viewProjection, derived.viewProjection, sizeof(block.viewProjection));

        // Directional cascaded shadows, one block per slot (see ShadowParams). The cascade
        // matrices, split distances and parameters come straight from the renderer. The
        // PCSS lanes are read only when specialized with VT_FEATURE_PCSS_SHADOWS, which
        // the renderer enables from the same shadow type; the sample counts belong to the
        // variant, so both slots carry them.
        const auto packDirectional = [&](const int slot, float* matrices, float* distances,
            float* params, float* params2, float* pcss, float* pcssRadii, float* pcssDepthRanges) -> Texture* {
            const auto& dir = shadowParams.directional[slot];
            const bool on = derived.directionalActive[slot];
            if (on) {
                std::memcpy(matrices, dir.shadowMatrixPalette, sizeof(dir.shadowMatrixPalette));
                std::memcpy(distances, dir.shadowCascadeDistances, sizeof(dir.shadowCascadeDistances));
            }
            // 0 = off, 1 = PCF depth compare, 2 = EVSM moments (Chebyshev).
            params[0] = on ? (shadowParams.vsm ? 2.0f : 1.0f) : 0.0f;
            params[1] = static_cast<float>(dir.numCascades);
            params[2] = dir.bias;
            params[3] = dir.strength;
            params2[0] = dir.normalBias;
            params2[1] = dir.cascadeBlend;
            pcss[0] = static_cast<float>(shadowParams.pcssSamples);
            pcss[1] = static_cast<float>(shadowParams.pcssBlockerSamples);
            pcss[2] = dir.penumbraSize;
            pcss[3] = dir.penumbraFalloff;
            std::memcpy(pcssRadii, dir.pcssCascadeRadii, sizeof(dir.pcssCascadeRadii));
            std::memcpy(pcssDepthRanges, dir.pcssCascadeDepthRanges, sizeof(dir.pcssCascadeDepthRanges));
            return on ? dir.shadowMap : nullptr;
        };
        textures.shadowMap[0] = packDirectional(0, block.shadowMatrices, block.shadowCascadeDistances,
            block.shadowParams, block.shadowParams2, block.pcssParams, block.pcssCascadeRadii,
            block.pcssCascadeDepthRanges);
        textures.shadowMap[1] = packDirectional(1, block.dirShadow1Matrices, block.dirShadow1CascadeDistances,
            block.dirShadow1Params, block.dirShadow1Params2, block.dirShadow1PcssParams,
            block.dirShadow1PcssCascadeRadii, block.dirShadow1PcssCascadeDepthRanges);
        // shadowParams2's other half carries two unrelated values.
        block.shadowParams2[2] = static_cast<float>(toneMapping);
        block.shadowParams2[3] = enableNormalMaps ? 1.0f : 0.0f;

        // Local light shadows (spot 2D + omni cube), up to 2 casters; each light's
        // coneParams[3] carries its slot. Omni params: near, far, relative bias, intensity.
        const auto packLocal = [](const DerivedLocalShadow& ls, float* matrix, float* params,
                                  float* omni, float* pcss) {
            std::memcpy(matrix, ls.matrix, sizeof(ls.matrix));
            std::memcpy(params, ls.params, sizeof(ls.params));
            std::memcpy(pcss, ls.pcss, sizeof(ls.pcss));
            if (ls.active && ls.isOmni) {
                omni[0] = ls.omniNear;
                omni[1] = ls.omniFar;
                omni[2] = ls.omniBias;
                omni[3] = ls.params[2];
            }
        };
        packLocal(derived.localShadows[0], block.localShadowMatrix0, block.localShadowParams0,
            block.omniShadowParams0, block.localShadowPcss0);
        packLocal(derived.localShadows[1], block.localShadowMatrix1, block.localShadowParams1,
            block.omniShadowParams1, block.localShadowPcss1);
        for (int slot = 0; slot < 2; ++slot) {
            textures.localShadowMap[slot] = derived.localShadows[slot].spotMap;
            textures.localVsm[slot] = derived.localShadows[slot].vsm;
            textures.omniShadowCube[slot] = derived.localShadows[slot].omniMap;
        }

        std::memcpy(block.ambient, derived.ambient, sizeof(derived.ambient));
        cameraPosition.store(block.cameraPosExposure);
        block.cameraPosExposure[3] = exposure;

        constexpr uint32_t kMaxLights = static_cast<uint32_t>(std::size(block.lights));
        block.lightCount[0] = derived.lightCount;
        for (uint32_t i = 0; i < kMaxLights; ++i) {
            GpuLightBlock& dst = block.lights[i];
            if (i >= derived.lightCount) {
                dst = GpuLightBlock{};
                dst.colorIntensity[3] = 0.0f;  // zero intensity → contributes nothing
                continue;
            }
            const DerivedLight& light = derived.lights[i];
            const GpuLightData& src = *light.source;
            src.position.store(dst.positionRange);
            dst.positionRange[3] = src.range;
            src.direction.store(dst.directionType);
            dst.directionType[3] = static_cast<float>(static_cast<uint32_t>(src.type));
            dst.colorIntensity[0] = light.linearColor[0];
            dst.colorIntensity[1] = light.linearColor[1];
            dst.colorIntensity[2] = light.linearColor[2];
            dst.colorIntensity[3] = light.intensity;
            dst.coneParams[0] = src.innerConeCos;
            dst.coneParams[1] = src.outerConeCos;
            dst.coneParams[2] = src.falloffModeLinear ? 1.0f : 0.0f;
            // Shadow slot: -1 = no shadow, else the local (or directional) slot.
            dst.coneParams[3] = src.castShadows ? static_cast<float>(src.shadowMapIndex) : -1.0f;
            // An area source: the world half axes, and the LightShape in the width's w.
            src.areaHalfWidth.store(dst.areaRightHalfWidth);
            dst.areaRightHalfWidth[3] = static_cast<float>(src.shape);
            src.areaHalfHeight.store(dst.areaUpHalfHeight);
            dst.areaUpHalfHeight[3] = 0.0f;
            dst.cookieFlags[0] = (src.cookieIndex >= 0 && src.cookie) ? 1.0f : 0.0f;
            dst.cookieFlags[1] = (src.cookieIndex >= 0) ? static_cast<float>(src.cookieIndex) : 0.0f;
            dst.cookieFlags[2] = static_cast<float>(src.cookieChannel);
            dst.cookieFlags[3] = src.cookieFalloff ? 1.0f : 0.0f;
        }

        // Light cookies: two 2D (spot) and two cube (omni) slots.
        const auto packCookie = [](const DerivedCookieSlot& slot, Texture*& texture, float* matrix, float* params) {
            texture = slot.texture;
            std::memcpy(matrix, slot.matrix, sizeof(slot.matrix));
            std::memcpy(params, slot.params, sizeof(slot.params));
        };
        packCookie(derived.cookie2D[0], textures.cookie2D[0], block.cookieMatrix2D0, block.cookieParams2D0);
        packCookie(derived.cookie2D[1], textures.cookie2D[1], block.cookieMatrix2D1, block.cookieParams2D1);
        std::memcpy(block.cookieTransform2D, derived.cookie2D[0].transform, sizeof(derived.cookie2D[0].transform));
        std::memcpy(block.cookieTransform2D + 4, derived.cookie2D[1].transform, sizeof(derived.cookie2D[1].transform));
        packCookie(derived.cookieCube[0], textures.cookieCube[0], block.cookieMatrixCube0, block.cookieParamsCube0);
        packCookie(derived.cookieCube[1], textures.cookieCube[1], block.cookieMatrixCube1, block.cookieParamsCube1);

        std::memcpy(block.fogColorDensity, derived.fogColorDensity, sizeof(derived.fogColorDensity));
        std::memcpy(block.fogStartEndType, derived.fogStartEndType, sizeof(derived.fogStartEndType));
    }
}
