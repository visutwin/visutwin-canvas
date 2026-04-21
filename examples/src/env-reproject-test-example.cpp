// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Equivalence smoke test for GPU vs CPU environment-atlas mipmap bake.
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

    int calcLevels(int size)
    {
        return 1 + static_cast<int>(std::floor(std::log2(std::max(size, 1))));
    }

    struct Rect { int x, y, w, h; };

    std::vector<Rect> mipmapRects(int size)
    {
        std::vector<Rect> rects;
        const int levels = calcLevels(256) - calcLevels(4);
        int rectX = 0, rectY = 0, rectW = size, rectH = size / 2;
        for (int i = 0; i <= levels; ++i) {
            if (rectW < 1 || rectH < 1) break;
            rects.push_back({rectX, rectY, rectW, rectH});
            rectX += rectH;
            rectY += rectH;
            rectW = std::max(1, rectW / 2);
            rectH = std::max(1, rectH / 2);
        }
        return rects;
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
            static_cast<NS::UInteger>(w * 4),
            region,
            0);
        return data;
    }

    struct DiffStats {
        double meanAbs = 0.0;
        int maxAbs = 0;
        size_t pixelCount = 0;
    };

    DiffStats diffRect(const std::vector<uint8_t>& cpu,
                       const std::vector<uint8_t>& gpu,
                       int atlasSize, const Rect& r)
    {
        DiffStats s;
        long long sum = 0;
        for (int y = 0; y < r.h; ++y) {
            for (int x = 0; x < r.w; ++x) {
                const size_t idx = (static_cast<size_t>(r.y + y) * atlasSize + (r.x + x)) * 4;
                for (int c = 0; c < 4; ++c) {
                    const int d = std::abs(static_cast<int>(cpu[idx + c]) - static_cast<int>(gpu[idx + c]));
                    sum += d;
                    if (d > s.maxAbs) s.maxAbs = d;
                    ++s.pixelCount;
                }
            }
        }
        s.meanAbs = s.pixelCount ? static_cast<double>(sum) / static_cast<double>(s.pixelCount) : 0.0;
        return s;
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
    if (!swapchain) {
        spdlog::error("SDL_GetRenderMetalLayer returned null");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto devicePtr = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!devicePtr) {
        spdlog::error("createGraphicsDevice failed");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
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

    auto* cpuAtlas = EnvLighting::generateAtlas(device.get(), sourceTex.get(),
        kAtlasSize, 32, 32, /*useGpu=*/false);
    if (!cpuAtlas) {
        spdlog::error("CPU generateAtlas returned null");
        return 1;
    }
    const auto cpuSize = cpuAtlas->getLevelDataSize(0);
    std::vector<uint8_t> cpuBytes(cpuSize);
    std::memcpy(cpuBytes.data(), cpuAtlas->getLevel(0), cpuSize);

    auto* gpuAtlas = EnvLighting::generateAtlas(device.get(), sourceTex.get(),
        kAtlasSize, 32, 32, /*useGpu=*/true);
    if (!gpuAtlas) {
        spdlog::error("GPU generateAtlas returned null");
        return 1;
    }
    const auto gpuBytes = readbackAtlas(gpuAtlas);
    if (gpuBytes.size() != cpuBytes.size()) {
        spdlog::error("GPU atlas readback size mismatch: cpu={} gpu={}",
            cpuBytes.size(), gpuBytes.size());
        return 1;
    }

    const auto rects = mipmapRects(kAtlasSize);

    // CPU bake uses seamPixels=2, GPU uses seamPixels=1 — they disagree inside
    // the 1-2 pixel border bands, dominating the mean on small rects.
    constexpr double kMeanThreshold = 15.0;
    constexpr int    kMaxThreshold  = 80;

    bool allPass = true;
    int rectIdx = 0;
    for (const auto& r : rects) {
        const auto s = diffRect(cpuBytes, gpuBytes, kAtlasSize, r);
        const bool pass = s.meanAbs < kMeanThreshold && s.maxAbs < kMaxThreshold;
        std::printf("rect[%d] (%d,%d %dx%d): mean=%.3f max=%d pixels=%zu  %s\n",
            rectIdx, r.x, r.y, r.w, r.h, s.meanAbs, s.maxAbs, s.pixelCount,
            pass ? "PASS" : "FAIL");
        const int cx = r.x + r.w / 2;
        const int cy = r.y + r.h / 2;
        const size_t idx = (static_cast<size_t>(cy) * kAtlasSize + cx) * 4;
        std::printf("   sample[%d,%d] cpu=(%3d %3d %3d %3d) gpu=(%3d %3d %3d %3d)\n",
            cx, cy,
            cpuBytes[idx+0], cpuBytes[idx+1], cpuBytes[idx+2], cpuBytes[idx+3],
            gpuBytes[idx+0], gpuBytes[idx+1], gpuBytes[idx+2], gpuBytes[idx+3]);
        if (!pass) allPass = false;
        ++rectIdx;
    }

    delete cpuAtlas;
    delete gpuAtlas;

    std::printf("%s\n", allPass ? "env-reproject-test: PASS" : "env-reproject-test: FAIL");

    device.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return allPass ? 0 : 1;
}
