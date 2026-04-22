// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Smoke test for the GPU environment-atlas bake. Builds an atlas from a
// synthetic equirect source and verifies the output is non-zero across the
// three sections (mipmap, GGX, Lambert).
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include <spdlog/spdlog.h>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/metal/metalTexture.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/envLighting.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kSrcWidth = 256;
    constexpr int kSrcHeight = 128;
    constexpr int kAtlasSize = 256;

    std::vector<float> makeSyntheticEquirect(int w, int h)
    {
        std::vector<float> data(static_cast<size_t>(w) * h * 4, 0.0f);
        for (int y = 0; y < h; ++y) {
            const float v = (static_cast<float>(y) + 0.5f) / h;
            for (int x = 0; x < w; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / w;
                const float skyMix = 1.0f - v;
                const float horizon = 1.0f - std::abs(v - 0.5f) * 2.0f;
                const float r = 0.1f + 0.4f * skyMix + 0.3f * horizon * (1.0f - u);
                const float g = 0.2f + 0.3f * skyMix + 0.4f * horizon;
                const float b = 0.5f + 0.4f * skyMix + 0.1f * u;
                const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                data[i + 0] = r;
                data[i + 1] = g;
                data[i + 2] = b;
                data[i + 3] = 1.0f;
            }
        }
        return data;
    }

    struct AtlasRect { int x, y, w, h; };

    bool rectNonZero(const std::vector<uint8_t>& atlas, int size, const AtlasRect& r)
    {
        for (int y = 0; y < r.h; ++y) {
            for (int x = 0; x < r.w; ++x) {
                const size_t idx = (static_cast<size_t>(r.y + y) * size + (r.x + x)) * 4;
                if (atlas[idx + 0] || atlas[idx + 1] || atlas[idx + 2]) return true;
            }
        }
        return false;
    }

    std::vector<uint8_t> readbackAtlas(Texture* atlas)
    {
        auto* hw = dynamic_cast<gpu::MetalTexture*>(atlas->impl());
        if (!hw || !hw->raw()) {
            spdlog::error("readbackAtlas: texture has no Metal impl");
            return {};
        }
        MTL::Texture* tex = hw->raw();
        const int w = static_cast<int>(tex->width());
        const int h = static_cast<int>(tex->height());
        std::vector<uint8_t> data(static_cast<size_t>(w) * h * 4, 0);
        MTL::Region region = MTL::Region::Make2D(0, 0,
            static_cast<NS::UInteger>(w), static_cast<NS::UInteger>(h));
        tex->getBytes(data.data(),
            static_cast<NS::UInteger>(w * 4), region, 0);
        return data;
    }
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window* window = SDL_CreateWindow("env-reproject-test", 64, 64,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    auto devicePtr = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    auto device = std::shared_ptr<GraphicsDevice>(std::move(devicePtr));

    const auto srcData = makeSyntheticEquirect(kSrcWidth, kSrcHeight);
    TextureOptions srcOptions;
    srcOptions.name = "syntheticEquirect";
    srcOptions.width = kSrcWidth;
    srcOptions.height = kSrcHeight;
    srcOptions.format = PixelFormat::PIXELFORMAT_RGBA32F;
    srcOptions.mipmaps = false;
    srcOptions.minFilter = FilterMode::FILTER_LINEAR;
    srcOptions.magFilter = FilterMode::FILTER_LINEAR;
    auto sourceTex = std::make_unique<Texture>(device.get(), srcOptions);
    sourceTex->setLevelData(0,
        reinterpret_cast<const uint8_t*>(srcData.data()),
        srcData.size() * sizeof(float));
    sourceTex->upload();

    auto* atlas = EnvLighting::generateAtlas(device.get(), sourceTex.get(),
        kAtlasSize, 32, 32);
    if (!atlas) {
        spdlog::error("generateAtlas returned null");
        return 1;
    }
    const auto bytes = readbackAtlas(atlas);

    std::vector<std::pair<std::string, AtlasRect>> sections = {
        {"mip[0]",  {0, 0, kAtlasSize, kAtlasSize / 2}},
        {"ggx[1]",  {0, kAtlasSize / 2, kAtlasSize / 2, kAtlasSize / 4}},
        {"lambert", {kAtlasSize / 4, kAtlasSize / 2 + kAtlasSize / 4,
                     kAtlasSize / 8, kAtlasSize / 16}},
    };

    bool allPass = true;
    for (const auto& [name, r] : sections) {
        const bool ok = rectNonZero(bytes, kAtlasSize, r);
        std::printf("%-8s (%d,%d %dx%d): %s\n",
            name.c_str(), r.x, r.y, r.w, r.h, ok ? "PASS" : "FAIL (all zeros)");
        if (!ok) allPass = false;
    }

    std::printf("%s\n", allPass ? "env-reproject-test: PASS" : "env-reproject-test: FAIL");

    delete atlas;
    device.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return allPass ? 0 : 1;
}
