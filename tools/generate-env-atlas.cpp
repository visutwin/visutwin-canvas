// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Offline tool: Generate RGBP env-atlas PNG from an HDR equirectangular source.
//
// Usage: generate-env-atlas <input.hdr> <output.png> [size=512]
//
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
// Declarations only — STB_IMAGE_WRITE_IMPLEMENTATION lives in the engine's
// platform/graphics/screenshot.cpp, which this tool links against.
#include "stb_image_write.h"

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>
#include <Metal/Metal.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/envLighting.h"

#include <spdlog/spdlog.h>

using namespace visutwin::canvas;

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

    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(false);
    float* hdrPixels = stbi_loadf(inputPath, &width, &height, &channels, 0);
    if (!hdrPixels || width <= 0 || height <= 0) {
        spdlog::error("Failed to load HDR file: {}", inputPath);
        if (hdrPixels) stbi_image_free(hdrPixels);
        return 1;
    }
    spdlog::info("Loaded {}x{} HDR ({} channels)", width, height, channels);

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<float> rgbaData(pixelCount * 4);
    for (size_t i = 0; i < pixelCount; ++i) {
        rgbaData[i * 4 + 0] = hdrPixels[i * channels + 0];
        rgbaData[i * 4 + 1] = channels > 1 ? hdrPixels[i * channels + 1] : hdrPixels[i * channels + 0];
        rgbaData[i * 4 + 2] = channels > 2 ? hdrPixels[i * channels + 2] : hdrPixels[i * channels + 0];
        rgbaData[i * 4 + 3] = 1.0f;
    }
    stbi_image_free(hdrPixels);

    // Headless-ish GPU init: a hidden SDL window with a Metal layer gives us
    // the CAMetalLayer the engine needs for device construction.
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("generate-env-atlas", 64, 64,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL);
    if (!window) {
        spdlog::error("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        spdlog::error("SDL_CreateRenderer failed: {}", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    auto devicePtr = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!devicePtr) {
        spdlog::error("createGraphicsDevice failed");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    auto device = std::shared_ptr<GraphicsDevice>(std::move(devicePtr));

    TextureOptions srcOptions;
    srcOptions.name = "hdrSource";
    srcOptions.width = static_cast<uint32_t>(width);
    srcOptions.height = static_cast<uint32_t>(height);
    srcOptions.format = PixelFormat::PIXELFORMAT_RGBA32F;
    srcOptions.mipmaps = false;
    srcOptions.minFilter = FilterMode::FILTER_LINEAR;
    srcOptions.magFilter = FilterMode::FILTER_LINEAR;
    auto sourceTex = std::make_unique<Texture>(device.get(), srcOptions);
    sourceTex->setLevelData(0,
        reinterpret_cast<const uint8_t*>(rgbaData.data()),
        rgbaData.size() * sizeof(float));
    sourceTex->upload();

    spdlog::info("Generating {}x{} RGBP atlas...", atlasSize, atlasSize);
    // The bake must run INSIDE a frame even though it draws nothing to the
    // screen. Its quad draws take their uniforms from the per-draw ring buffers,
    // which are frame-scoped: frameStart is what hands out a region to write
    // into. Without it every quad draw in the bake read the same unwritten block
    // and the atlas came out one flat colour — with the rect layout visibly
    // correct, which is what made it look like a readback problem for so long.
    device->frameStart();
    auto* atlas = EnvLighting::generateAtlas(device.get(), sourceTex.get(), atlasSize);
    device->frameEnd();
    if (!atlas) {
        spdlog::error("Atlas generation failed");
        return 1;
    }

    // Read the atlas back through the public seam. This used to call
    // MTL::Texture::getBytes on the atlas directly, which is a device-private
    // render target: getBytes does not fail on one, it returns whatever is
    // mapped, so the tool wrote a plausible-looking wrong PNG rather than an
    // obviously broken one. Texture::read blits through shared staging.
    std::vector<uint8_t> atlasData;
    if (!atlas->read(atlasData)) {
        spdlog::error("Atlas readback failed");
        return 1;
    }

    spdlog::info("Writing atlas to: {}", outputPath);
    const int result = stbi_write_png(outputPath, atlasSize, atlasSize, 4,
                                       atlasData.data(), atlasSize * 4);
    if (!result) {
        spdlog::error("Failed to write PNG: {}", outputPath);
        return 1;
    }

    spdlog::info("Done! Generated {}x{} RGBP atlas ({} bytes)",
                 atlasSize, atlasSize, atlasData.size());

    delete atlas;
    device.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
