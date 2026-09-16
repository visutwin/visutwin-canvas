// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Texture VRAM accounting is a running total: Texture's constructor adds
// TextureUtils::calcGpuSize and its destructor subtracts it, so an error here does
// not read as a wrong number once — it makes the total DRIFT, and only while
// textures are being created and destroyed. Nothing visual shows that.
//
// The whole texture side of DeviceVRAM was dead until 2026-09-16 (_gpuSize was
// declared and never assigned, so adjustVramSizeTracking was never reached), which
// is why these are closed-form cases checked by hand rather than against a
// reference implementation: there was none.

#include <cstdint>
#include <iostream>
#include <string>

#include "platform/graphics/constants.h"
#include "platform/graphics/textureUtils.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void expect(const std::string& name, const size_t actual, const size_t expected)
    {
        const bool ok = actual == expected;
        std::cout << (ok ? "  ok   " : "  FAIL ") << name << ": " << actual
                  << (ok ? "" : " (expected " + std::to_string(expected) + ")") << '\n';
        if (!ok) {
            ++failures;
        }
    }
}

int main()
{
    std::cout << "texture gpu size\n";

    // ── Uncompressed, single level ───────────────────────────────────────
    // 4 bytes per texel, no mips, one slice.
    expect("RGBA8 64x64 level 0",
        TextureUtils::calcGpuSize(64, 64, 1, 1, false, 0, PixelFormat::PIXELFORMAT_RGBA8),
        64u * 64u * 4u);

    // ── A full mip chain sums to 4/3 of the base, EXACTLY ────────────────
    // 64x64 has 7 levels (64,32,16,8,4,2,1). The classic 1 + 1/4 + 1/16 ... series
    // terminates, so the total is an integer and can be written down: 5461 texels.
    {
        size_t texels = 0;
        for (uint32_t d = 64; d >= 1; d /= 2) {
            texels += static_cast<size_t>(d) * d;
            if (d == 1) break;
        }
        expect("RGBA8 64x64 full mip chain",
            TextureUtils::calcGpuSize(64, 64, 1, 7, false, 0, PixelFormat::PIXELFORMAT_RGBA8),
            texels * 4u);
        expect("  (that chain is 5461 texels)", texels, 5461u);
    }

    // ── Cubemap multiplies by six faces ──────────────────────────────────
    expect("RGBA8 32x32 cubemap, 1 level",
        TextureUtils::calcGpuSize(32, 32, 1, 1, true, 0, PixelFormat::PIXELFORMAT_RGBA8),
        32u * 32u * 4u * 6u);

    // ── Array length multiplies slices, and 0 means ONE slice ────────────
    // Texture::isArray() is `_arrayLength > 0`, so a non-array texture arrives here
    // with 0 and must not be measured as zero bytes.
    expect("RGBA8 16x16 arrayLength 0 == 1 slice",
        TextureUtils::calcGpuSize(16, 16, 1, 1, false, 0, PixelFormat::PIXELFORMAT_RGBA8),
        16u * 16u * 4u);
    expect("RGBA8 16x16 arrayLength 4",
        TextureUtils::calcGpuSize(16, 16, 1, 1, false, 4, PixelFormat::PIXELFORMAT_RGBA8),
        16u * 16u * 4u * 4u);

    // ── Block-compressed: whole blocks, including partial edge blocks ────
    // DXT1 is 4x4 blocks at 8 bytes. 64x64 is 16x16 blocks.
    expect("DXT1 64x64 level 0",
        TextureUtils::calcGpuSize(64, 64, 1, 1, false, 0, PixelFormat::PIXELFORMAT_DXT1),
        16u * 16u * 8u);

    // The edge case that bytes-per-pixel gets wrong: a 1x1 mip of a block format
    // still costs a FULL block, so the tail of a compressed mip chain does not
    // shrink toward zero.
    expect("DXT1 1x1 costs a whole block",
        TextureUtils::calcGpuSize(1, 1, 1, 1, false, 0, PixelFormat::PIXELFORMAT_DXT1),
        8u);

    // 5x5 ASTC over a 12x12 image: ceil(12/5) = 3 blocks each way, 16 bytes each.
    expect("ASTC_5x5 12x12 rounds up to 3x3 blocks",
        TextureUtils::calcGpuSize(12, 12, 1, 1, false, 0, PixelFormat::PIXELFORMAT_ASTC_5x5),
        3u * 3u * 16u);

    // A compressed format must never be measured through bytes-per-pixel, which is
    // deliberately 0 for them — that would report every compressed texture as free.
    expect("compressed formats are not sized as 0",
        TextureUtils::calcGpuSize(256, 256, 1, 1, false, 0, PixelFormat::PIXELFORMAT_ASTC_4x4) > 0
            ? 1u : 0u,
        1u);

    // ── Volume textures shrink in three dimensions ───────────────────────
    // 8x8x8 with 2 levels: 8^3 + 4^3 texels.
    expect("RGBA8 8x8x8 volume, 2 levels",
        TextureUtils::calcGpuSize(8, 8, 8, 2, false, 0, PixelFormat::PIXELFORMAT_RGBA8),
        (8u * 8u * 8u + 4u * 4u * 4u) * 4u);

    // ── An undescribed format answers 0 rather than guessing ─────────────
    // DEPTHSTENCIL's byte size is a backend probe (D24S8 or D32S8), so there is no
    // right answer here. Zero is the safe one: a guessed size added at creation and
    // unwound from a different guess at release is what makes a total drift.
    expect("DEPTHSTENCIL is not guessed",
        TextureUtils::calcGpuSize(64, 64, 1, 1, false, 0, PixelFormat::PIXELFORMAT_DEPTHSTENCIL),
        0u);

    // ── numLevels is honoured as given, not recomputed ───────────────────
    // Texture clamps and may be handed an explicit count; if this recomputed the
    // chain itself, the add and the subtract could disagree.
    expect("explicit numLevels 3 of 64x64",
        TextureUtils::calcGpuSize(64, 64, 1, 3, false, 0, PixelFormat::PIXELFORMAT_RGBA8),
        (64u * 64u + 32u * 32u + 16u * 16u) * 4u);

    if (failures == 0) {
        std::cout << "texture gpu size: all checks passed\n";
        return 0;
    }
    std::cout << "texture gpu size: " << failures << " check(s) FAILED\n";
    return 1;
}
