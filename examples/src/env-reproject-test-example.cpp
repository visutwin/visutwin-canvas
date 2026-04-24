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
#include "scene/graphics/envReproject.h"

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

    // CPU reference: build cube face N (RGBA32F) from an equirect source
    // using the original CPU port's faceUvToDir + dirToEquirectUv + manual
    // bilinear convention. Used to A/B against the GPU equirect-to-cube path.
    std::vector<float> cpuBuildCubeFace(int face, int faceSize,
        const float* srcData, int srcW, int srcH)
    {
        std::vector<float> out(static_cast<size_t>(faceSize) * faceSize * 4, 0.0f);

        auto sampleEquirect = [&](float u, float v, float& r, float& g, float& b) {
            u = u - std::floor(u);
            v = std::clamp(v, 0.0f, 1.0f);
            const float fx = u * static_cast<float>(srcW - 1);
            const float fy = v * static_cast<float>(srcH - 1);
            int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, srcW - 1);
            int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, srcH - 1);
            int x1 = (x0 + 1) % srcW;
            int y1 = std::min(y0 + 1, srcH - 1);
            float sx = fx - x0;
            float sy = fy - y0;
            auto pix = [&](int px, int py) -> const float* {
                return &srcData[(py * srcW + px) * 4];
            };
            const float* p00 = pix(x0, y0);
            const float* p10 = pix(x1, y0);
            const float* p01 = pix(x0, y1);
            const float* p11 = pix(x1, y1);
            float vals[3];
            for (int c = 0; c < 3; ++c) {
                float top = p00[c] * (1 - sx) + p10[c] * sx;
                float bot = p01[c] * (1 - sx) + p11[c] * sx;
                vals[c] = top * (1 - sy) + bot * sy;
            }
            r = vals[0]; g = vals[1]; b = vals[2];
        };

        auto faceUvToDir = [](int f, float u, float v, float& x, float& y, float& z) {
            const float sc = u * 2.0f - 1.0f;
            const float tc = v * 2.0f - 1.0f;
            switch (f) {
                case 0: x =  1.0f; y = -tc;   z = -sc;   break;
                case 1: x = -1.0f; y = -tc;   z =  sc;   break;
                case 2: x =  sc;   y =  1.0f; z =  tc;   break;
                case 3: x =  sc;   y = -1.0f; z = -tc;   break;
                case 4: x =  sc;   y = -tc;   z =  1.0f; break;
                default: x = -sc;  y = -tc;   z = -1.0f; break;
            }
            float len = std::sqrt(x * x + y * y + z * z);
            if (len > 0) { x /= len; y /= len; z /= len; }
        };

        constexpr float PI = 3.14159265358979323846f;
        for (int py = 0; py < faceSize; ++py) {
            for (int px = 0; px < faceSize; ++px) {
                float u = (px + 0.5f) / faceSize;
                float v = (py + 0.5f) / faceSize;
                float dx, dy, dz;
                faceUvToDir(face, u, v, dx, dy, dz);
                float phi = std::atan2(dx, dz);
                float theta = std::asin(std::clamp(dy, -1.0f, 1.0f));
                float eu = phi / (2.0f * PI) + 0.5f;
                float ev = 1.0f - (theta / PI + 0.5f);
                float r, g, b;
                sampleEquirect(eu, ev, r, g, b);
                size_t idx = (static_cast<size_t>(py) * faceSize + px) * 4;
                out[idx + 0] = r;
                out[idx + 1] = g;
                out[idx + 2] = b;
                out[idx + 3] = 1.0f;
            }
        }
        return out;
    }

    // Read mip level `mip`, slice `face`, of a cubemap texture as RGBA32F.
    std::vector<float> readbackCubeFace(Texture* cube, int face, int mip = 0)
    {
        auto* hw = dynamic_cast<gpu::MetalTexture*>(cube->impl());
        if (!hw || !hw->raw()) return {};
        MTL::Texture* tex = hw->raw();
        const int baseSize = static_cast<int>(tex->width());
        const int mipSize = std::max(1, baseSize >> mip);
        std::vector<float> out(static_cast<size_t>(mipSize) * mipSize * 4, 0.0f);
        MTL::Region region = MTL::Region::Make2D(0, 0,
            static_cast<NS::UInteger>(mipSize), static_cast<NS::UInteger>(mipSize));
        const NS::UInteger bytesPerRow = static_cast<NS::UInteger>(mipSize * 4 * sizeof(float));
        const NS::UInteger bytesPerImage = bytesPerRow * mipSize;
        tex->getBytes(out.data(), bytesPerRow, bytesPerImage, region,
            static_cast<NS::UInteger>(mip),
            static_cast<NS::UInteger>(face));
        return out;
    }

    // CPU 2x2 box downsample one mip level: floats in/out, RGBA, src=2x dst.
    std::vector<float> cpuDownsampleBox(const std::vector<float>& src, int srcSize)
    {
        const int dstSize = std::max(1, srcSize / 2);
        std::vector<float> dst(static_cast<size_t>(dstSize) * dstSize * 4, 0.0f);
        for (int y = 0; y < dstSize; ++y) {
            for (int x = 0; x < dstSize; ++x) {
                const int sx = x * 2;
                const int sy = y * 2;
                const int sx1 = std::min(sx + 1, srcSize - 1);
                const int sy1 = std::min(sy + 1, srcSize - 1);
                const size_t dIdx = (static_cast<size_t>(y) * dstSize + x) * 4;
                for (int c = 0; c < 4; ++c) {
                    float v00 = src[(static_cast<size_t>(sy)  * srcSize + sx)  * 4 + c];
                    float v10 = src[(static_cast<size_t>(sy)  * srcSize + sx1) * 4 + c];
                    float v01 = src[(static_cast<size_t>(sy1) * srcSize + sx)  * 4 + c];
                    float v11 = src[(static_cast<size_t>(sy1) * srcSize + sx1) * 4 + c];
                    dst[dIdx + c] = (v00 + v10 + v01 + v11) * 0.25f;
                }
            }
        }
        return dst;
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
    auto sourceTex = std::make_shared<Texture>(device.get(), srcOptions);
    sourceTex->setLevelData(0,
        reinterpret_cast<const uint8_t*>(srcData.data()),
        srcData.size() * sizeof(float));
    sourceTex->upload();

    // ── Cubemap face-0 byte-by-byte diagnostic ─────────────────────────
    // Builds face 0 on GPU and on CPU (using the original convention) and
    // compares per-channel diffs. If the GPU face is byte-identical to CPU,
    // face orientation/handedness is correct and any visual gap is elsewhere
    // (mipmap filter, etc.).
    {
        constexpr int kFaceSize = 256;
        auto gpuCube = equirectToCubemap(device.get(), sourceTex, kFaceSize, false);
        const auto gpuFace0 = readbackCubeFace(gpuCube.get(), 0);
        const auto cpuFace0 = cpuBuildCubeFace(0, kFaceSize,
            srcData.data(), kSrcWidth, kSrcHeight);

        if (gpuFace0.size() != cpuFace0.size() || gpuFace0.empty()) {
            std::printf("face0-diag: size mismatch gpu=%zu cpu=%zu\n",
                gpuFace0.size(), cpuFace0.size());
        } else {
            double sumAbs = 0.0;
            float maxAbs = 0.0f;
            int maxIdx = 0;
            for (size_t i = 0; i < gpuFace0.size(); ++i) {
                float d = std::abs(gpuFace0[i] - cpuFace0[i]);
                sumAbs += d;
                if (d > maxAbs) { maxAbs = d; maxIdx = static_cast<int>(i); }
            }
            const double meanAbs = sumAbs / gpuFace0.size();
            const int mp = maxIdx / 4;
            const int mc = maxIdx % 4;
            const int my = mp / kFaceSize;
            const int mx = mp % kFaceSize;
            std::printf("face0-diag: mean=%.6f max=%.6f at (%d,%d) chan=%d\n",
                meanAbs, maxAbs, mx, my, mc);
            // Spot-check 4 corners + center.
            auto sample = [&](int x, int y) {
                size_t b = (static_cast<size_t>(y) * kFaceSize + x) * 4;
                std::printf("  (%3d,%3d) cpu=(%.4f %.4f %.4f) gpu=(%.4f %.4f %.4f)\n",
                    x, y,
                    cpuFace0[b+0], cpuFace0[b+1], cpuFace0[b+2],
                    gpuFace0[b+0], gpuFace0[b+1], gpuFace0[b+2]);
            };
            sample(0, 0);
            sample(kFaceSize - 1, 0);
            sample(0, kFaceSize - 1);
            sample(kFaceSize - 1, kFaceSize - 1);
            sample(kFaceSize / 2, kFaceSize / 2);
        }

        // Compare mip-chain filtering: GPU's blit-generated mips vs CPU's
        // explicit 2x2 box downsample of mip 0.
        std::vector<float> cpuMip = cpuFace0;
        int cpuMipSize = kFaceSize;
        for (int targetMip = 1; targetMip <= 6; ++targetMip) {
            cpuMip = cpuDownsampleBox(cpuMip, cpuMipSize);
            cpuMipSize = std::max(1, cpuMipSize / 2);

            const auto gpuMip = readbackCubeFace(gpuCube.get(), 0, targetMip);
            if (gpuMip.size() != cpuMip.size() || gpuMip.empty()) {
                std::printf("face0-mip%d: size mismatch gpu=%zu cpu=%zu\n",
                    targetMip, gpuMip.size(), cpuMip.size());
                continue;
            }
            double sumAbs = 0.0;
            float maxAbs = 0.0f;
            for (size_t i = 0; i < gpuMip.size(); ++i) {
                float d = std::abs(gpuMip[i] - cpuMip[i]);
                sumAbs += d;
                if (d > maxAbs) maxAbs = d;
            }
            std::printf("face0-mip%d (%dx%d): mean=%.6f max=%.6f\n",
                targetMip, cpuMipSize, cpuMipSize,
                sumAbs / gpuMip.size(), maxAbs);
        }
    }

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
