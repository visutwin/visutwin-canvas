// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// THE per-pass lighting block, one layout for every backend: Metal binds it at buffer 4,
// Vulkan at set 2 binding 0, and the forward program declares it field for field under these
// names (`LightingData` in engine/shaders/slang/forward/forward-fragment-head.slang; the
// bundle's reflected size is held against sizeof(LightingBlock) by slangBundleTests). Every member is a vec4 or a run of
// exactly four 4-byte scalars, so the natural C++ layout already satisfies std140 and
// Metal's constant layout, and the size is a multiple of 16; a mismatch shifts every
// field that follows it. deriveLighting decides every value, packLightingBlock lays it
// out, and each backend only uploads it (and keeps the few fields it owns itself:
// the grab flags, the reflection blur, the cluster grid, the debug pass).
//
#pragma once

#include <cstddef>
#include <cstdint>

namespace visutwin::canvas
{
    struct DerivedLighting;
    struct ShadowParams;
    struct Vector3;
    class Texture;

    /// One light of the main array (GLSL / MSL `Light`). 112 bytes.
    struct GpuLightBlock
    {
        float positionRange[4]  = {0.0f, 0.0f, 0.0f, 0.0f};   // xyz position, w range
        float directionType[4]  = {0.0f, -1.0f, 0.0f, 0.0f};  // xyz direction, w type (0 dir, 1 point, 2 spot)
        float colorIntensity[4] = {1.0f, 1.0f, 1.0f, 0.0f};   // rgb linear colour, w intensity
        // innerCos, outerCos, falloffLinear, shadow slot (-1 = none; a local slot 0/1,
        // or a directional light's directional slot)
        float coneParams[4]     = {1.0f, 1.0f, 1.0f, -1.0f};
        // An area source: xyz the world half-width axis, w the LightShape (0 punctual,
        // 1 rect, 2 disk, 3 sphere); xyz the world half-height axis.
        float areaRightHalfWidth[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float areaUpHalfHeight[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        // Light cookie: x=hasCookie, y=slot in the 2D or cube pool (the light type
        // picks which), z=CookieChannel, w=cookieFalloff (spot only).
        float cookieFlags[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };

    /// Env-atlas encoding tag stored in LightingBlock::envParams[2].
    enum class EnvAtlasEncoding : uint32_t
    {
        Srgb = 0u,
        Rgbp = 1u,
        Rgbm = 2u,
    };

    struct LightingBlock
    {
        float ambient[4]            = {0.0f, 0.0f, 0.0f, 0.0f};  // rgb ambient
        float cameraPosExposure[4]  = {0.0f, 0.0f, 0.0f, 1.0f};  // xyz camera, w exposure
        uint32_t lightCount[4]      = {0u, 0u, 0u, 0u};          // x = active light count
        GpuLightBlock lights[8];
        float fogColorDensity[4]    = {0.0f, 0.0f, 0.0f, 0.0f};  // rgb fog color, w density
        float fogStartEndType[4]    = {10.0f, 100.0f, 0.0f, 0.0f}; // start, end, type(0/1/2), pad
        // x=skyboxIntensity, y=hasEnvAtlas(0/1), z=EnvAtlasEncoding, w=skyboxMip
        float envParams[4]          = {1.0f, 0.0f, 0.0f, 0.0f};

        // Directional cascaded shadows, slot 0. shadowMatrices is 4 column-major mat4
        // (`mat4 shadowMatrices[4]`) — each maps world space to a cascade's shadow-atlas
        // UV + depth. Matches ShadowParams::shadowMatrixPalette.
        float shadowMatrices[64]       = {};
        float shadowCascadeDistances[4]= {0.0f, 0.0f, 0.0f, 0.0f}; // per-cascade far split (view depth)
        // x=enabled (0 off, 1 PCF depth compare, 2 EVSM moments), y=numCascades, z=depthBias, w=strength
        float shadowParams[4]          = {0.0f, 1.0f, 0.0001f, 1.0f};
        // x=normalBias, y=cascadeBlend, z=toneMapping mode, w=enableNormalMaps
        float shadowParams2[4]         = {0.0f, 0.0f, 0.0f, 0.0f};

        // Directional PCSS (SHADOW_PCSS_32F), read only when the shader is specialized
        // with VT_FEATURE_PCSS_SHADOWS. The world-space penumbra math needs each cascade's
        // ortho half-extent and caster depth span.
        // x=filterSamples, y=blockerSamples, z=penumbraSize, w=penumbraFalloff
        float pcssParams[4]            = {16.0f, 16.0f, 1.0f, 1.0f};
        float pcssCascadeRadii[4]       = {1.0f, 1.0f, 1.0f, 1.0f};
        float pcssCascadeDepthRanges[4] = {1.0f, 1.0f, 1.0f, 1.0f};

        // Local light shadows (spot 2D + omni cubemap), up to 2 casters. Spot slots use a
        // per-light VP matrix (world → shadow UV + depth); omni slots use a distance
        // compare against the cubemap (no matrix).
        float localShadowMatrix0[16]   = {};   // spot slot 0 world → atlas UV+depth
        float localShadowMatrix1[16]   = {};   // spot slot 1
        // x=depthBias, y=normalBias, z=intensity, w=isOmni(0/1)
        float localShadowParams0[4]    = {0.0001f, 0.0f, 1.0f, 0.0f};
        float localShadowParams1[4]    = {0.0001f, 0.0f, 1.0f, 0.0f};
        // Omni cubemap params. x=near, y=far, z=RELATIVE depthBias, w=intensity
        float omniShadowParams0[4]     = {0.01f, 100.0f, 0.0001f, 1.0f};
        float omniShadowParams1[4]     = {0.01f, 100.0f, 0.0001f, 1.0f};
        // Local-light PCSS, per slot, a runtime branch (no shader variant): x = blocker-
        // search radius in shadow-map UV, 0 = PCSS off for that slot. y=near, z=far, w=1 for VSM.
        float localShadowPcss0[4]      = {0.0f, 0.01f, 100.0f, 0.0f};
        float localShadowPcss1[4]      = {0.0f, 0.01f, 100.0f, 0.0f};

        // Light cookies, two slots per kind (spot 2D, omni cubemap). Spot slots carry a
        // world → cookie-UV projection; omni slots carry the light's world transform,
        // whose rotation maps light→fragment into cube space.
        float cookieMatrix2D0[16]      = {};
        float cookieMatrix2D1[16]      = {};
        float cookieMatrixCube0[16]    = {};
        float cookieMatrixCube1[16]    = {};
        // x=intensity, y=cookieFalloff, z=CookieChannel, w=pad
        float cookieParams2D0[4]       = {1.0f, 1.0f, 0.0f, 0.0f};
        float cookieParams2D1[4]       = {1.0f, 1.0f, 0.0f, 0.0f};
        float cookieParamsCube0[4]     = {1.0f, 1.0f, 0.0f, 0.0f};
        float cookieParamsCube1[4]     = {1.0f, 1.0f, 0.0f, 0.0f};

        // xyz = sky dome center (world), w = flags: bit0 = has skybox cubemap,
        // bit1 = dome projection (view dir from dome center, not camera).
        float skyParams2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        // Nine premultiplied irradiance SH coefficients.
        float ambientSH[9][4] = {};
        float clusterBoundsMin[4] = {};
        float clusterBoundsRange[4] = {};
        float clusterCellsCountByBoundsSize[4] = {};
        uint32_t clusterParams[4] = {};
        uint32_t clusterParams2[4] = {};
        float reflectionProbeBoxMin[4] = {};
        float reflectionProbeBoxMax[4] = {};
        float reflectionProbePosition[4] = {};
        float reflectionProbeParams[4] = {};

        // Screen-space reflections: the camera view-projection used to project a marched
        // world-space position to screen UV, and the clip planes needed to linearize the
        // sampled scene depth. z/w flag whether the scene colour and depth grabs are bound
        // (Vulkan: an unbound separate image samples white; Metal samples zero and
        // leaves them 0): refraction needs the colour grab, the SSR march needs both.
        float viewProjection[16] = {};
        float cameraNearFar[4]   = {0.1f, 1000.0f, 0.0f, 0.0f};

        // Nishita atmosphere (96 bytes = 6 vec4), the Scene's atmosphere block copied in
        // by setAtmosphereUniforms. Scene-global and uploaded only when dirty, so it costs
        // nothing measurable here and no binding of its own. Defaults are the Scene's.
        float atmoPlanetCenterAndRadius[4]  = {0.0f, 0.0f, 0.0f, 6371000.0f};
        float atmoRadiusAndSunIntensity[4]  = {6471000.0f, 22.0f, 0.9998f, 0.0f};
        float atmoRayleighCoeffAndScale[4]  = {5.5e-6f, 13.0e-6f, 22.4e-6f, 8500.0f};
        float atmoMieCoeffAndScale[4]       = {21.0e-6f, 1200.0f, 0.758f, 0.0f};
        float atmoSunDirection[4]           = {0.0f, 1.0f, 0.0f, 0.0f};
        float atmoCameraAltitudeAndParams[4] = {0.0f, 32.0f, 8.0f, 0.0f};

        // Blurred planar reflection. Defaults match ReflectionBlurParams so an unset
        // device behaves the same. xy = 1/viewport, zw = viewport.
        float screenInvResolution[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
        // x = intensity, y = blurAmount, z = fadeStrength, w = angleFade
        float reflectionParams[4]       = {1.0f, 0.0f, 1.0f, 0.5f};
        float reflectionFadeColor[4]    = {0.5f, 0.5f, 0.5f, 0.0f};
        // x = planeDistance, y = heightRange (depth-pass distance normalization),
        // z = colour map bound, w = depth map bound
        float reflectionDepthParams[4]  = {0.0f, 10.0f, 0.0f, 0.0f};

        // [0] is a bitfield: bit 5 = the forward pass writes linear HDR for a camera
        // frame (compose applies exposure, tone mapping and gamma). [1] carries the
        // DebugShaderPass mode as a plain value — one compiled variant serves every mode.
        uint32_t flagsAndPad[4] = {};

        // Second directional shadow slot: the slot-0 fields above for the light whose
        // shadow index (coneParams.w) is 1. Same meaning lane for lane (the PCSS sample
        // counts, being the variant's, are the same in both slots).
        float dirShadow1Matrices[64]              = {};
        float dirShadow1CascadeDistances[4]       = {0.0f, 0.0f, 0.0f, 0.0f};
        float dirShadow1Params[4]                 = {0.0f, 1.0f, 0.0001f, 1.0f};
        float dirShadow1Params2[4]                = {0.0f, 0.0f, 0.0f, 0.0f};
        float dirShadow1PcssParams[4]             = {16.0f, 16.0f, 1.0f, 1.0f};
        float dirShadow1PcssCascadeRadii[4]       = {1.0f, 1.0f, 1.0f, 1.0f};
        float dirShadow1PcssCascadeDepthRanges[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        // Scene::skyboxRotation, one column per vec4 (w of the first column says
        // "rotated"): environment samples read along R * dir.
        float skyboxRotation[12]                  = {1.0f, 0.0f, 0.0f, 0.0f,
                                                     0.0f, 1.0f, 0.0f, 0.0f,
                                                     0.0f, 0.0f, 1.0f, 0.0f};
        // Spot cookie 2x2 per 2D cookie slot, mat2 columns.
        float cookieTransform2D[8]                = {1.0f, 0.0f, 0.0f, 1.0f,
                                                     1.0f, 0.0f, 0.0f, 1.0f};
        // Blue-noise jitter: xy offset the opacity dither per frame while the camera
        // jitters (TAA), zero otherwise.
        float ditherJitter[4]                     = {0.0f, 0.0f, 0.0f, 0.0f};
    };
    static_assert(sizeof(GpuLightBlock) == 112);
    static_assert(sizeof(LightingBlock) == 2896, "the shaders of both backends declare this size");
    static_assert(sizeof(LightingBlock) % 16 == 0);

    /// The six atmosphere vec4s as one span, for setAtmosphereUniforms: first member
    /// through last, NOT "offset to end of struct".
    inline constexpr size_t kLightingBlockAtmosphereOffset = offsetof(LightingBlock, atmoPlanetCenterAndRadius);
    inline constexpr size_t kLightingBlockAtmosphereBytes =
        offsetof(LightingBlock, atmoCameraAltitudeAndParams) + sizeof(LightingBlock::atmoCameraAltitudeAndParams) -
        kLightingBlockAtmosphereOffset;
    static_assert(kLightingBlockAtmosphereBytes == 96);

    /// The textures the block's values refer to, for the backend to bind: one per slot,
    /// null where the slot is unused.
    struct LightingBlockTextures
    {
        Texture* shadowMap[2] = {nullptr, nullptr};        // directional slots 0 and 1
        Texture* localShadowMap[2] = {nullptr, nullptr};   // spot slots (EVSM moments when localVsm)
        bool localVsm[2] = {false, false};
        Texture* omniShadowCube[2] = {nullptr, nullptr};
        Texture* cookie2D[2] = {nullptr, nullptr};
        Texture* cookieCube[2] = {nullptr, nullptr};
    };

    /// Lays `derived` out in the block: the ambient, SH and view-projection, the main light
    /// array, fog, both directional shadow slots, the local shadows, the cookies, the
    /// camera position and exposure, and the two scalar lanes of shadowParams2. Fields a
    /// backend owns (grab flags, reflection blur, clusters, debug pass, environment, sky
    /// rotation, dither jitter, atmosphere) are left as they are.
    void packLightingBlock(LightingBlock& block, LightingBlockTextures& textures, const DerivedLighting& derived,
        const ShadowParams& shadowParams, const Vector3& cameraPosition, float exposure, int toneMapping,
        bool enableNormalMaps);
}
