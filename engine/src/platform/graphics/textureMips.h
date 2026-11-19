// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// The mip chain of an 8-bit texture, computed on the CPU (Texture::upload builds it for a 2D
// texture that asks for mipmaps and has only its top level). Each level is the 2x2 box average
// of the level above, rounded to the nearest count. An sRGB texture's colour channels are
// averaged in LINEAR light (decoded, averaged, encoded), as the hardware generates the mips of an
// sRGB format; its alpha and every channel of any other texture are averaged as stored.
//
// The GPU generators this replaces for such textures (Metal's generateMipmaps blit, and Vulkan's
// blit through MoltenVK) average the stored values and drift a count or two over a deep chain:
// a 2048 concrete texture's mean fell from 192.8 to 191.0 at 1 x 1, darkening every surface seen
// from far away (a probe bake's 8 x 8 faces read 2-3% low).
//
#pragma once

#include <cstdint>
#include <vector>

namespace visutwin::canvas
{
    /// Levels 1 .. levels - 1 of a mip chain whose level 0 is `level0` (width x height texels of
    /// `channels` bytes, tightly packed); each next level is max(1, size / 2) on each axis. With
    /// `srgb`, the first three channels are averaged in linear light (a fourth, alpha, as stored).
    std::vector<std::vector<uint8_t>> generateMipChain8(const uint8_t* level0, uint32_t width, uint32_t height,
        uint32_t channels, bool srgb, uint32_t levels);
}
