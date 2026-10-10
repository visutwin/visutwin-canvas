// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// The lighting block's SEMANTIC derivation, shared by both backends.
//
// `MetalUniformBinder::LightingUniforms` and `VulkanLightingUBO` are genuinely different
// layouts (Metal keeps atmosphere in its own buffer, the per-light structs differ, most shared
// fields sit at different indices), so each backend still packs its own block. What they must
// NOT each decide for themselves is what goes into it: the sRGB-to-linear decode of every
// colour, which light goes into which cookie slot, what an unused shadow or cookie slot holds,
// the omni shadow's near plane and relative bias, and when a directional slot counts as active.
// Decided once per backend, they drift apart (an absent view-projection becoming identity on one
// and zeros on the other, a directional slot active on one whenever it is counted and on the
// other only when it also has a map). They are decided once, here, and each backend's
// setLightingUniforms only copies the result into its layout.
//
#pragma once

#include <cstdint>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    struct DerivedLight
    {
        const GpuLightData* source = nullptr;   // the renderer's light, for fields copied as they are
        // The colour and intensity the shader multiplies, from lightRadiance (lightRadiance.h):
        // below an intensity of 1 the colour is scaled before it is decoded and the
        // intensity is 1, so pack these two, never the source's colour and intensity.
        float linearColor[3] = {0.0f, 0.0f, 0.0f};
        float intensity = 0.0f;
    };

    struct DerivedLocalShadow
    {
        bool active = false;
        bool isOmni = false;
        Texture* spotMap = nullptr;     // a spot light's 2D depth map (EVSM moments when vsm)
        bool vsm = false;               // spot VSM_16F: distance-ratio moments, not depth
        Texture* omniMap = nullptr;     // an omni light's depth cube
        float matrix[16] = {};          // spot: world -> shadow clip, column-major; else zeros
        // bias, normal bias, intensity, 1 for omni; an unused slot holds {0.0001, 0, 1, 0}.
        float params[4] = {0.0001f, 0.0f, 1.0f, 0.0f};
        // Omni: near, far (the renderer stores it in the view-projection's [0][0]), and the
        // RELATIVE bias, a fraction of the receiver distance applied before the projection:
        // perspective depth is crushed against 1.0 at cube shadow ranges, so a fixed offset
        // after the projection erases omni shadows outright.
        float omniNear = 0.0f;
        float omniFar = 0.0f;
        float omniBias = 0.0f;
        float pcss[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // search area (0 = off), near, far, 1 = VSM
    };

    struct DerivedCookieSlot
    {
        Texture* texture = nullptr;
        float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float params[4] = {1.0f, 1.0f, 0.0f, 0.0f};   // intensity, falloff, channel
        float transform[4] = {1.0f, 0.0f, 0.0f, 1.0f}; // 2D only: the cookie 2x2, mat2 columns
    };

    struct DerivedLighting
    {
        float ambient[4] = {};              // linear
        float ambientSH[9][4] = {};         // zeros when no probe
        float viewProjection[16] = {};      // identity when none is given

        uint32_t lightCount = 0;            // at most the backend's light capacity
        std::vector<DerivedLight> lights;

        float fogColorDensity[4] = {};      // linear colour, density
        float fogStartEndType[4] = {};      // start, end, FogType (0 when fog is off)

        bool directionalActive[ShadowParams::kMaxDirectionalShadows] = {};

        // The clustered-light mask bit the draw accepts (clusterParams2.y on both
        // backends): MASK_AFFECT_DYNAMIC when the draw's mesh-instance mask has it, else
        // MASK_AFFECT_LIGHTMAPPED. A clustered light is shaded only when its own mask
        // (GpuClusteredLight areaHalfHeight.w) has this bit, which is what the main array
        // gets by filtering on the mask; clustered lights share one grid per light set, so
        // they are filtered in the shader instead.
        uint32_t clusterLightAccept = MASK_AFFECT_DYNAMIC;

        static constexpr float kOmniShadowBias = 0.002f;
        DerivedLocalShadow localShadows[ShadowParams::kMaxLocalShadows];

        // Two 2D (spot) and two cube (omni) cookie slots; a light's cookieIndex picks the slot
        // within the pool its type selects.
        DerivedCookieSlot cookie2D[2];
        DerivedCookieSlot cookieCube[2];
    };

    /// The lighting block's values for one layer, for a backend holding at most `maxLights`
    /// lights in its main array. `meshLightMask` is the mask of the draws the block serves
    /// (the renderer re-issues the block when a draw's mask differs).
    DerivedLighting deriveLighting(const Color& ambientColor, const std::vector<GpuLightData>& lights,
        size_t maxLights, const FogParams& fogParams, const ShadowParams& shadowParams,
        const Vector3* ambientSH, const Matrix4* viewProjection,
        uint32_t meshLightMask = MASK_AFFECT_DYNAMIC);
}
