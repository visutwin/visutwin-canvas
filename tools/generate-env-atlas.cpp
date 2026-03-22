// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Offline tool: Generate RGBP env-atlas PNG from an HDR equirectangular source.
//
// Usage: generate-env-atlas <input.hdr> <output.png> [size=512]
//
// The output is a 512x512 (or custom size) RGBA8 PNG with RGBP encoding,
// ready for use with scene->setEnvAtlas() in the engine examples.
//

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "scene/graphics/envLighting.h"
#include <spdlog/spdlog.h>

int main(int argc, char* argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.hdr> <output.png> [size=512]\n", argv[0]);
        return 1;
    }

    const char* inputPath = argv[1];
    const char* outputPath = argv[2];
    const int atlasSize = (argc >= 4) ? atoi(argv[3]) : 512;

    spdlog::set_level(spdlog::level::info);
    spdlog::info("Loading HDR: {}", inputPath);

    // Load HDR equirectangular source
    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(false);
    float* hdrPixels = stbi_loadf(inputPath, &width, &height, &channels, 0);

    if (!hdrPixels || width <= 0 || height <= 0) {
        spdlog::error("Failed to load HDR file: {}", inputPath);
        if (hdrPixels) stbi_image_free(hdrPixels);
        return 1;
    }

    spdlog::info("Loaded {}x{} HDR ({} channels)", width, height, channels);

    // Convert to RGBA float (stbi may return RGB for HDR)
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<float> rgbaData(pixelCount * 4);
    for (size_t i = 0; i < pixelCount; ++i) {
        rgbaData[i * 4 + 0] = hdrPixels[i * channels + 0];
        rgbaData[i * 4 + 1] = channels > 1 ? hdrPixels[i * channels + 1] : hdrPixels[i * channels + 0];
        rgbaData[i * 4 + 2] = channels > 2 ? hdrPixels[i * channels + 2] : hdrPixels[i * channels + 0];
        rgbaData[i * 4 + 3] = 1.0f;
    }
    stbi_image_free(hdrPixels);

    // Generate atlas (CPU-only, no GPU device needed)
    spdlog::info("Generating {}x{} RGBP atlas...", atlasSize, atlasSize);
    auto atlasData = visutwin::canvas::EnvLighting::generateAtlasRaw(
        rgbaData.data(), width, height, atlasSize);

    if (atlasData.empty()) {
        spdlog::error("Atlas generation failed");
        return 1;
    }

    // Write PNG
    spdlog::info("Writing atlas to: {}", outputPath);
    const int result = stbi_write_png(outputPath, atlasSize, atlasSize, 4,
                                       atlasData.data(), atlasSize * 4);

    if (!result) {
        spdlog::error("Failed to write PNG: {}", outputPath);
        return 1;
    }

    spdlog::info("Done! Generated {}x{} RGBP atlas ({} bytes)",
                 atlasSize, atlasSize, atlasData.size());
    return 0;
}
