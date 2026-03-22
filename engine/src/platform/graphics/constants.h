// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.09.2025.
//
#pragma once

#include <cstdint>

namespace visutwin::canvas
{
    enum BufferUsage {
        BUFFER_STATIC = 0,
        BUFFER_DYNAMIC = 1,
        BUFFER_STREAM = 2
    };

    enum class FilterMode : uint32_t {
        FILTER_NEAREST                  = 0, // Point sample filtering
        FILTER_LINEAR                   = 1, // Bilinear filtering
        FILTER_NEAREST_MIPMAP_NEAREST   = 2, // Use the nearest neighbor in the nearest mipmap level
        FILTER_NEAREST_MIPMAP_LINEAR    = 3, // Linearly interpolate in the nearest mipmap level
        FILTER_LINEAR_MIPMAP_NEAREST    = 4, // Use the nearest neighbor after linearly interpolating between mipmap levels
        FILTER_LINEAR_MIPMAP_LINEAR     = 5  // Linearly interpolate both the mipmap levels and between texels
    };

    enum class TextureProjection : uint32_t {
        TEXTUREPROJECTION_NONE = 0, // Texture data is not stored in a specific projection format
        TEXTUREPROJECTION_CUBE = 1  // Texture data is stored in the cubemap projection format
    };

    enum class PixelFormat : uint32_t {
        PIXELFORMAT_RGB8 = 6,           // 24-bit RGB (8-bits for a red channel, 8 for green and 8 for blue)
        PIXELFORMAT_RGBA8 = 7,          // 32-bit RGBA (8-bits for a red channel, 8 for green, 8 for blue with 8-bit alpha)
        PIXELFORMAT_RGBA16F = 12,       // 16-bit floating point RGBA (16-bit float for each red, green, blue and alpha channel)
        PIXELFORMAT_RGBA32F = 14,       // 32-bit floating point RGBA (32-bit float for each red, green, blue and alpha channel)
        PIXELFORMAT_R32F = 15,          // 32-bit floating point single channel format
        PIXELFORMAT_DEPTHSTENCIL = 19,  // A readable depth/stencil buffer format
        PIXELFORMAT_DEPTH = 16,         // A readable depth buffer format
        PIXELFORMAT_R8 = 52,            // 8-bit per-channel (R) format
        PIXELFORMAT_RG8 = 53,           // 8-bit per-channel (RG) format
        PIXELFORMAT_DEPTH16 = 69        // A 16-bit depth buffer format
    };

    enum class TexHint
    {
        TEXHINT_NONE = 0,
        TEXHINT_SHADOWMAP = 1,
        TEXHINT_ASSET = 2,
        TEXHINT_LIGHTMAP = 3
    };

    enum class CullMode {
        CULLFACE_NONE = 0,          // No triangles are culled
        CULLFACE_BACK = 1,          // Triangles facing away from the view direction are culled
        CULLFACE_FRONT = 2,         // Triangles facing the view direction are culled
        CULLFACE_FRONTANDBACK = 3   // Triangles are culled regardless of their orientation with respect to the view direction.
                                    // Note that point or line primitives are unaffected by this render state
    };

    // Texture addressing modes
    enum AddressMode
    {
        ADDRESS_REPEAT          = 0, // Ignores the integer part of texture coordinates, using only the fractional part.
        ADDRESS_CLAMP_TO_EDGE   = 1, // Clamps texture coordinate to the range 0 to 1.
        ADDRESS_MIRRORED_REPEAT = 2  // Texture coordinate to be set to the fractional part if the integer part is even.
                                     // If the integer part is odd, then the texture coordinate is set to 1 minus the fractional part.
    };

    // Texture properties
    enum TextureProperty
    {
        TEXPROPERTY_MIN_FILTER      = 1,
        TEXPROPERTY_MAG_FILTER      = 2,
        TEXPROPERTY_ADDRESS_U       = 4,
        TEXPROPERTY_ADDRESS_V       = 8,
        TEXPROPERTY_ADDRESS_W       = 16,
        TEXPROPERTY_COMPARE_ON_READ = 32,
        TEXPROPERTY_COMPARE_FUNC    = 64,
        TEXPROPERTY_ANISOTROPY      = 128,
        TEXPROPERTY_ALL             = 255
    };

    bool isCompressedPixelFormat(PixelFormat format);

    bool isIntegerPixelFormat(PixelFormat format);
}
