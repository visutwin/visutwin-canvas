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
        SKYTYPE_DOME
    };

    enum class LightType
    {
        LIGHTTYPE_DIRECTIONAL, // Directional (global) light source
        LIGHTTYPE_OMNI,        // Omni-directional (local) light source
        LIGHTTYPE_POINT,       // Point (local) light source
        LIGHTTYPE_SPOT,        // Spot (local) light source
        LIGHTTYPE_AREA_RECT    // Rectangular area light (local)
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
