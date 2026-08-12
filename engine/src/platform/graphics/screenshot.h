// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Backbuffer capture helpers shared by the graphics backends.
//
#pragma once

#include <cstdint>
#include <string>

namespace visutwin::canvas
{
    /**
     * Writes a captured backbuffer to a PNG.
     *
     * `pixels` is tightly packed or `rowPitch`-strided 8-bit RGBA/BGRA. Both
     * backends hand over BGRA (the swapchain/drawable format on macOS), so
     * `swapRedBlue` reorders in place-free fashion while copying. Alpha is
     * forced opaque: a rendered frame routinely leaves alpha at whatever the
     * shader wrote, which would make the PNG look transparent in a viewer.
     *
     * Returns false and logs on failure; never throws.
     */
    bool writeScreenshotPng(const std::string& path, uint32_t width, uint32_t height,
        uint32_t rowPitch, const uint8_t* pixels, bool swapRedBlue);
}
