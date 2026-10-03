// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// The lighting block's semantic derivation (see lightingDerivation.h).
//
#include "lightingDerivation.h"

#include <algorithm>
#include <cstring>

#include "lightRadiance.h"

namespace visutwin::canvas
{
    DerivedLighting deriveLighting(const Color& ambientColor, const std::vector<GpuLightData>& lights,
        const size_t maxLights, const FogParams& fogParams, const ShadowParams& shadowParams,
        const Vector3* ambientSH, const Matrix4* viewProjection, const uint32_t meshLightMask)
    {
        DerivedLighting out;

        // A draw on a dynamic mesh accepts the clustered lights that affect dynamic meshes;
        // any other draw (a lightmapped mesh, a bake) the ones that affect lightmapped meshes.
        out.clusterLightAccept = (meshLightMask & MASK_AFFECT_DYNAMIC) != 0u
            ? MASK_AFFECT_DYNAMIC : MASK_AFFECT_LIGHTMAPPED;

        // Ambient SH light probes: nine premultiplied irradiance coefficients.
        if (ambientSH) {
            for (int i = 0; i < 9; ++i) {
                ambientSH[i].store(out.ambientSH[i]);
                out.ambientSH[i][3] = 0.0f;
            }
        }

        // The camera view-projection for the fragment stage's screen projections (refraction,
        // SSR), column-major as Matrix4::store writes it.
        if (viewProjection) {
            viewProjection->store(out.viewProjection);
        } else {
            out.viewProjection[0] = out.viewProjection[5] = out.viewProjection[10] = out.viewProjection[15] = 1.0f;
        }

        // Colours are authored in sRGB and shaded in linear.
        Color ambientLinear;
        ambientLinear.linear(&ambientColor);
        out.ambient[0] = ambientLinear.r;
        out.ambient[1] = ambientLinear.g;
        out.ambient[2] = ambientLinear.b;

        out.lightCount = static_cast<uint32_t>(std::min(lights.size(), maxLights));
        out.lights.resize(out.lightCount);
        for (uint32_t i = 0; i < out.lightCount; ++i) {
            const GpuLightData& src = lights[i];
            DerivedLight& dst = out.lights[i];
            dst.source = &src;
            const LightRadiance radiance = lightRadiance(src.color, src.intensity);
            dst.linearColor[0] = radiance.linearColor[0];
            dst.linearColor[1] = radiance.linearColor[1];
            dst.linearColor[2] = radiance.linearColor[2];
            dst.intensity = radiance.intensity;
        }

        Color fogLinear;
        fogLinear.linear(&fogParams.color);
        out.fogColorDensity[0] = fogLinear.r;
        out.fogColorDensity[1] = fogLinear.g;
        out.fogColorDensity[2] = fogLinear.b;
        out.fogColorDensity[3] = fogParams.density;
        out.fogStartEndType[0] = fogParams.start;
        out.fogStartEndType[1] = fogParams.end;
        out.fogStartEndType[2] = fogParams.enabled ? static_cast<float>(fogParams.type) : 0.0f;

        // A directional slot is active when the renderer counted it AND gave it a map.
        for (int slot = 0; slot < ShadowParams::kMaxDirectionalShadows; ++slot) {
            out.directionalActive[slot] = slot < shadowParams.directionalCount &&
                                          shadowParams.directional[slot].shadowMap != nullptr;
        }

        for (int i = 0; i < ShadowParams::kMaxLocalShadows && i < shadowParams.localShadowCount; ++i) {
            const ShadowParams::LocalShadow& ls = shadowParams.localShadows[i];
            DerivedLocalShadow& dst = out.localShadows[i];
            dst.active = true;
            dst.isOmni = ls.isOmni;
            dst.vsm = ls.vsm && !ls.isOmni;
            if (ls.isOmni) {
                dst.omniMap = ls.shadowMap;
                dst.omniNear = DerivedLighting::kOmniShadowNear;
                dst.omniFar = ls.viewProjection.getElement(0, 0);
                dst.omniBias = DerivedLighting::kOmniShadowBias;
            } else {
                dst.spotMap = ls.shadowMap;
                ls.viewProjection.store(dst.matrix);
            }
            dst.params[0] = ls.bias;
            dst.params[1] = ls.normalBias;
            dst.params[2] = ls.intensity;
            dst.params[3] = ls.isOmni ? 1.0f : 0.0f;
            dst.pcss[0] = ls.pcssSearchArea;
            dst.pcss[1] = ls.nearClip;
            dst.pcss[2] = ls.farClip;
            dst.pcss[3] = ls.vsm ? 1.0f : 0.0f;
        }

        for (uint32_t i = 0; i < out.lightCount; ++i) {
            const GpuLightData& src = lights[i];
            if (!src.cookie || src.cookieIndex < 0 || src.cookieIndex > 1) {
                continue;
            }
            DerivedCookieSlot& slot = src.type == GpuLightType::Point ? out.cookieCube[src.cookieIndex]
                                                                       : out.cookie2D[src.cookieIndex];
            slot.texture = src.cookie;
            src.cookieMatrix.store(slot.matrix);
            slot.params[0] = src.cookieIntensity;
            slot.params[1] = src.cookieFalloff ? 1.0f : 0.0f;
            slot.params[2] = static_cast<float>(src.cookieChannel);
            std::memcpy(slot.transform, src.cookieTransform, sizeof(slot.transform));
            slot.params[3] = 0.0f;
        }
        return out;
    }
}
