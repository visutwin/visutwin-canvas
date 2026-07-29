// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.09.2025.
//
#pragma once

#include <cstdint>
#include <unordered_map>
#include <platform/graphics/constants.h>

namespace visutwin::canvas
{
    // The world layer (matches Engine bootstrap IDs).
    constexpr int LAYERID_WORLD = 1;

    // The depth layer (matches Engine bootstrap IDs).
    constexpr int LAYERID_DEPTH = 2;

    // The skybox layer (matches Engine bootstrap IDs).
    constexpr int LAYERID_SKYBOX = 3;

    // The UI layer (matches Engine bootstrap IDs).
    constexpr int LAYERID_UI = 4;

    // The immediate layer (matches Engine bootstrap IDs).
    constexpr int LAYERID_IMMEDIATE = 5;

    // Light mask bits.
    constexpr uint32_t MASK_NONE = 0u;
    constexpr uint32_t MASK_AFFECT_DYNAMIC = 1u;
    constexpr uint32_t MASK_AFFECT_LIGHTMAPPED = 2u;
    constexpr uint32_t MASK_BAKE = 4u;

    // Specular occlusion modes.
    constexpr uint32_t SPECOCC_NONE = 0u;
    constexpr uint32_t SPECOCC_AO = 1u;
    constexpr uint32_t SPECOCC_GLOSSDEPENDENT = 2u;

    enum SkyType
    {
        SKYTYPE_INFINITE,
        SKYTYPE_BOX,
        SKYTYPE_DOME,
        SKYTYPE_ATMOSPHERE   // Sphere mesh + infinite behavior (camera-following)
    };

    enum class LightType
    {
        LIGHTTYPE_DIRECTIONAL, // Directional (global) light source
        LIGHTTYPE_OMNI,        // Omni-directional (local) light source
        LIGHTTYPE_POINT,       // Point (local) light source
        LIGHTTYPE_SPOT,        // Spot (local) light source
        LIGHTTYPE_AREA_RECT    // Rectangular area light (local)
    };

    // Area light shape (upstream LIGHTSHAPE_*). Applies to LIGHTTYPE_AREA_RECT
    // lights; disk is inscribed in the width/height quad, sphere uses
    // max(halfWidth, halfHeight) as radius.
    enum class AreaLightShape
    {
        LIGHTSHAPE_RECT = 0,
        LIGHTSHAPE_DISK = 1,
        LIGHTSHAPE_SPHERE = 2
    };

    enum class LightFalloff
    {
        LIGHTFALLOFF_LINEAR = 0,
        LIGHTFALLOFF_INVERSESQUARED = 1
    };

    enum class MaskType
    {
        MASK_NONE = static_cast<int>(::visutwin::canvas::MASK_NONE),
        MASK_AFFECT_DYNAMIC = static_cast<int>(::visutwin::canvas::MASK_AFFECT_DYNAMIC),
        MASK_AFFECT_LIGHTMAPPED = static_cast<int>(::visutwin::canvas::MASK_AFFECT_LIGHTMAPPED),
        MASK_BAKE = static_cast<int>(::visutwin::canvas::MASK_BAKE)
    };

    enum class ShadowUpdateType
    {
        SHADOWUPDATE_NONE,      // The shadow map is not to be updated
        SHADOWUPDATE_THISFRAME, // The shadow map is regenerated this frame and not on subsequent frames
        SHADOWUPDATE_REALTIME   // The shadow map is regenerated every frame
    };

    // Tonemapping modes.
    enum Tonemap
    {
        TONEMAP_LINEAR = 0,
        TONEMAP_FILMIC = 1,
        TONEMAP_HEJL = 2,
        TONEMAP_ACES = 3,
        TONEMAP_ACES2 = 4,
        TONEMAP_NEUTRAL = 5,
        TONEMAP_NONE = 6,
    };

    // Opacity dither matrices (upstream DITHER_*). Selects the ordered-dither threshold pattern
    // used to render partial opacity in the opaque pass.
    //
    // DEVIATION: upstream also offers DITHER_BLUENOISE and DITHER_IGNNOISE; only the Bayer
    // matrices are ported, and the pattern is static (upstream jitters it per frame for TAA).
    enum class DitherMode
    {
        DITHER_NONE = 0,
        DITHER_BAYER2 = 1,
        DITHER_BAYER4 = 2,
        DITHER_BAYER8 = 3,
        DITHER_BAYER16 = 4,
    };

    // Debug shader passes (upstream SHADERPASS_ALBEDO and friends). Replaces the forward pass
    // output with a single surface quantity, to inspect what the material frontend produced.
    //
    // DEVIATION: upstream allocates a distinct named shader pass — and so a distinct compiled
    // shader — per debug mode. Here a single VT_FEATURE_DEBUG_PASS variant carries all of them
    // and the mode is a runtime uniform, so switching modes needs no shader recompile, and a
    // build that never enables a debug pass pays nothing (the block compiles out entirely).
    enum class DebugShaderPass
    {
        DEBUGPASS_NONE = 0,          // normal forward rendering
        DEBUGPASS_ALBEDO = 1,        // base color, gamma encoded
        DEBUGPASS_WORLDNORMAL = 2,   // world-space normal, remapped to [0, 1]
        DEBUGPASS_OPACITY = 3,
        DEBUGPASS_SPECULARITY = 4,   // F0
        DEBUGPASS_GLOSS = 5,
        DEBUGPASS_METALNESS = 6,
        DEBUGPASS_AO = 7,            // occlusion map only, not SSAO
        DEBUGPASS_EMISSION = 8,      // gamma encoded
        DEBUGPASS_LIGHTING = 9,      // full lighting over a neutral 0.5 albedo
        DEBUGPASS_UV0 = 10,          // uv0 in the red/green channels
    };

    enum class AspectRatioMode
    {
        ASPECT_AUTO,    // Automatically set an aspect ratio to current render target's width divided by height
        ASPECT_MANUAL,  // Use the manual aspect ratio value
    };

    enum ShadowType
    {
        /**
         * A shadow sampling technique using 32bit shadow map that averages depth comparisons from a 3x3
         * grid of texels for softened shadow edges.
         */
        SHADOW_PCF3_32F = 0,

        /**
         * A shadow sampling technique using a 16-bit exponential variance shadow map that leverages
         * variance to approximate shadow boundaries, enabling soft shadows. Only supported when
         * {@link GraphicsDevice#textureHalfFloatRenderable} is true. Falls back to {@link SHADOW_PCF3_32F},
         * if not supported.
         */
        SHADOW_VSM_16F = 2,

        /**
         * A shadow sampling technique using a 32-bit shadow map that performs a single depth
         * comparison for sharp shadow edges.
         */
        SHADOW_PCF1_32F = 5,

        /**
         * Percentage-closer soft shadows: contact-hardening penumbras driven by a
         * Vogel-disk blocker search. Directional lights only in this port.
         * DEVIATION: upstream renders a dedicated R32F linear-depth map; this port
         * samples the standard hardware depth map without comparison.
         */
        SHADOW_PCSS_32F = 6,
    };

    struct ShadowTypeInfo
    {
        const char* name;
        const char* kind;
        PixelFormat format;
        bool pcf = false;
        bool vsm = false;
    };

    extern const std::unordered_map<ShadowType, ShadowTypeInfo> shadowTypeInfo;
}
