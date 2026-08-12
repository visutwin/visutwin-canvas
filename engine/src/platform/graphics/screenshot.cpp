// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "screenshot.h"

#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    bool writeScreenshotPng(const std::string& path, const uint32_t width,
        const uint32_t height, const uint32_t rowPitch, const uint8_t* pixels,
        const bool swapRedBlue)
    {
        if (path.empty() || width == 0 || height == 0 || !pixels) {
            spdlog::error("Screenshot: nothing to write for '{}'", path);
            return false;
        }

        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u);
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* src = pixels + static_cast<size_t>(y) * rowPitch;
            uint8_t* dst = rgba.data() + static_cast<size_t>(y) * width * 4u;
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t b0 = src[x * 4u + 0u];
                const uint8_t b1 = src[x * 4u + 1u];
                const uint8_t b2 = src[x * 4u + 2u];
                dst[x * 4u + 0u] = swapRedBlue ? b2 : b0;
                dst[x * 4u + 1u] = b1;
                dst[x * 4u + 2u] = swapRedBlue ? b0 : b2;
                dst[x * 4u + 3u] = 255u;
            }
        }

        if (stbi_write_png(path.c_str(), static_cast<int>(width),
                static_cast<int>(height), 4, rgba.data(),
                static_cast<int>(width * 4u)) == 0) {
            spdlog::error("Screenshot: failed to write '{}'", path);
            return false;
        }
        spdlog::info("Screenshot written: {} ({}x{})", path, width, height);
        return true;
    }
}
