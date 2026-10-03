// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// Orientation of an environment atlas baked from an equirect, on a real device of
// whichever backend this build has. The equirect is dark except for one bright patch
// LEFT of centre and ABOVE the horizon, so a mirrored bake and an upside-down bake both
// put the patch where nothing bright should be.
//
// The contract: the atlas holds the source at the direction it came from, so the uv the
// shading lookups compute for a direction d (toSphericalUv(d), mapped into a rect) reads
// the equirect's texel at that same uv. The shaders negate X before that lookup, exactly
// as the skybox does before it samples its cube, so a reflection and the sky show the
// same environment. The skybox cube built from the same equirect is read back too: it
// must hold the patch along d, or the sky and the reflections disagree.
//
// It checks the unconvolved mip rect, the sharpest GGX rect and the Lambert rect, which
// covers both the reproject and the convolve passes. The bake goes through
// EnvLighting::generateAtlas, the path the examples use, and every result is read back
// with Texture::read.
//
// It needs a GPU and a window, so it carries the `gpu` label, which the CI test presets
// exclude; run it with `ctest --preset gpu` (or `ctest -L gpu` in a Vulkan build).
//
// The metal-cpp *_PRIVATE_IMPLEMENTATION translation unit of this executable: a program
// linking the static engine supplies it (see examples/exampleApp.cpp).
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <QuartzCore/QuartzCore.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/envLighting.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kPi = std::numbers::pi_v<float>;

    // The atlas layout the forward shaders read (common-utils: ATLAS_SIZE, ATLAS_SEAM).
    constexpr int kAtlasSize = 512;
    constexpr float kAtlasSeam = 1.0f / static_cast<float>(kAtlasSize);

    struct Dir
    {
        float x, y, z;
    };

    Dir fromAngles(const float azimuth, const float elevation)
    {
        const float c = std::cos(elevation);
        return {std::sin(azimuth) * c, std::sin(elevation), std::cos(azimuth) * c};
    }

    float dot(const Dir& a, const Dir& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    struct Uv
    {
        float u, v;
    };

    // toSphericalUv in common-utils: v = 0 is the top row, +Y.
    Uv toSphericalUv(const Dir& d)
    {
        const float azimuth = (d.x == 0.0f && d.z == 0.0f) ? 0.0f : std::atan2(d.x, d.z);
        const float elevation = std::asin(std::clamp(d.y, -1.0f, 1.0f));
        return {azimuth / (2.0f * kPi) + 0.5f, 1.0f - (elevation / kPi + 0.5f)};
    }

    // mapUv in common-utils, over a rect in atlas units, with the one-texel seam inset.
    Uv mapUv(const Uv uv, const float rx, const float ry, const float rw, const float rh)
    {
        return {rx + kAtlasSeam + (rw - 2.0f * kAtlasSeam) * uv.u, ry + kAtlasSeam + (rh - 2.0f * kAtlasSeam) * uv.v};
    }

    Uv mapRoughnessUv(const Uv uv, const float level)
    {
        const float t = 1.0f / std::exp2(level);
        return mapUv(uv, 0.0f, 1.0f - t, t, t * 0.5f);
    }

    Uv mapAmbientUv(const Uv uv)
    {
        const auto s = static_cast<float>(kAtlasSize);
        return mapUv(uv, 128.0f / s, 384.0f / s, 64.0f / s, 32.0f / s);
    }

    // The luminance-ish brightness of an RGBP texel: rgb * (8 - 7a), squared.
    float decodeRgbp(const uint8_t* texel)
    {
        const float scale = -static_cast<float>(texel[3]) / 255.0f * 7.0f + 8.0f;
        float sum = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float value = static_cast<float>(texel[c]) / 255.0f * scale;
            sum += value * value;
        }
        return sum / 3.0f;
    }

    float atlasAt(const std::vector<uint8_t>& atlas, const Uv atlasUv)
    {
        const int x = std::clamp(static_cast<int>(atlasUv.u * kAtlasSize), 0, kAtlasSize - 1);
        const int y = std::clamp(static_cast<int>(atlasUv.v * kAtlasSize), 0, kAtlasSize - 1);
        return decodeRgbp(&atlas[(static_cast<size_t>(y) * kAtlasSize + static_cast<size_t>(x)) * 4]);
    }

    // The face and face uv the hardware cube lookup takes for d: the inverse of the
    // face mapping the cube bakes write (faceUvToDir), whose face texel row 0 is the top.
    void cubeFaceUv(const Dir& d, uint32_t& face, Uv& uv)
    {
        const float ax = std::abs(d.x);
        const float ay = std::abs(d.y);
        const float az = std::abs(d.z);
        float sc = 0.0f;
        float tc = 0.0f;
        if (ax >= ay && ax >= az) {
            face = d.x > 0.0f ? 0u : 1u;
            sc = (d.x > 0.0f ? -d.z : d.z) / ax;
            tc = -d.y / ax;
        } else if (ay >= az) {
            face = d.y > 0.0f ? 2u : 3u;
            sc = d.x / ay;
            tc = (d.y > 0.0f ? d.z : -d.z) / ay;
        } else {
            face = d.z > 0.0f ? 4u : 5u;
            sc = (d.z > 0.0f ? d.x : -d.x) / az;
            tc = -d.y / az;
        }
        uv = {(sc + 1.0f) * 0.5f, (tc + 1.0f) * 0.5f};
    }

    std::string number(const float value) { return std::to_string(value); }
}

int main()
{
    const Backend backend = defaultBackend();
    if (backend == Backend::Metal) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }
    const SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | (backend == Backend::Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_METAL);
    SDL_Window* window = SDL_CreateWindow("env-atlas-orientation", 64, 64, flags);
    SDL_Renderer* renderer = backend == Backend::Metal ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!window || (backend == Backend::Metal && !renderer)) {
        std::cerr << "window or renderer failed: " << SDL_GetError() << '\n';
        return 1;
    }

    {
        GraphicsDeviceOptions options;
        options.backend = backend;
        options.window = window;
        if (renderer) {
            options.swapChain = SDL_GetRenderMetalLayer(renderer);
        }
        std::unique_ptr<GraphicsDevice> device = createGraphicsDevice(options);
        check(device != nullptr, std::string("a ") + backendName(backend) + " device is created");
        if (!device) {
            return 1;
        }

        // The patch: 12 degrees around azimuth -60 degrees (x < 0, so LEFT of the
        // equirect's centre, u = 1/3) and elevation +20 degrees (ABOVE the horizon).
        const Dir patch = fromAngles(-kPi / 3.0f, kPi / 9.0f);
        const Dir mirrored{-patch.x, patch.y, patch.z};    // what a left-right mirror reads
        const Dir upsideDown{patch.x, -patch.y, patch.z};  // what a vertical flip reads
        constexpr float kBright = 20.0f;
        constexpr float kDark = 0.05f;
        const float patchCos = std::cos(12.0f * kPi / 180.0f);

        // The equirect, laid out as toSphericalUv reads it: texel (i, j) holds the
        // direction whose uv is the texel centre.
        constexpr uint32_t kWidth = 128;
        constexpr uint32_t kHeight = 64;
        std::vector<float> pixels(static_cast<size_t>(kWidth) * kHeight * 4);
        for (uint32_t j = 0; j < kHeight; ++j) {
            for (uint32_t i = 0; i < kWidth; ++i) {
                const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(kWidth);
                const float v = (static_cast<float>(j) + 0.5f) / static_cast<float>(kHeight);
                const Dir d = fromAngles((u - 0.5f) * 2.0f * kPi, (0.5f - v) * kPi);
                const float value = dot(d, patch) > patchCos ? kBright : kDark;
                float* texel = &pixels[(static_cast<size_t>(j) * kWidth + i) * 4];
                texel[0] = texel[1] = texel[2] = value;
                texel[3] = 1.0f;
            }
        }
        {
            const Uv at = toSphericalUv(patch);
            check(at.u < 0.5f && at.v < 0.5f, "the patch sits left of centre and above the horizon of the equirect");
        }

        TextureOptions sourceOptions;
        sourceOptions.name = "env-atlas-orientation-equirect";
        sourceOptions.width = kWidth;
        sourceOptions.height = kHeight;
        sourceOptions.format = PixelFormat::PIXELFORMAT_RGBA32F;
        sourceOptions.mipmaps = false;
        sourceOptions.numLevels = 1;
        sourceOptions.minFilter = FilterMode::FILTER_LINEAR;
        sourceOptions.magFilter = FilterMode::FILTER_LINEAR;
        auto source = std::make_unique<Texture>(device.get(), sourceOptions);
        source->setLevelData(0, reinterpret_cast<const uint8_t*>(pixels.data()), pixels.size() * sizeof(float));
        source->upload();

        // ── The atlas ─────────────────────────────────────────────────────────
        std::unique_ptr<Texture> atlasTexture(EnvLighting::generateAtlas(device.get(), source.get(), kAtlasSize));
        std::vector<uint8_t> atlas;
        const bool readAtlas = atlasTexture && atlasTexture->read(atlas);
        check(readAtlas && atlas.size() == static_cast<size_t>(kAtlasSize) * kAtlasSize * 4,
            "the atlas is baked and reads back");
        if (readAtlas) {
            // The unconvolved environment (the mip rect at level 0) is the equirect itself.
            const float mipPatch = atlasAt(atlas, mapRoughnessUv(toSphericalUv(patch), 0.0f));
            const float mipMirrored = atlasAt(atlas, mapRoughnessUv(toSphericalUv(mirrored), 0.0f));
            const float mipUpsideDown = atlasAt(atlas, mapRoughnessUv(toSphericalUv(upsideDown), 0.0f));
            check(mipPatch > 5.0f, "mip rect: the patch is bright where the lookup reads its direction (" +
                number(mipPatch) + ")");
            check(mipMirrored < 1.0f, "mip rect: the mirrored direction is dark, so the bake is not mirrored (" +
                number(mipMirrored) + ")");
            check(mipUpsideDown < 1.0f, "mip rect: the direction below the horizon is dark, so the bake is not "
                "upside down (" + number(mipUpsideDown) + ")");

            // The sharpest GGX level: the patch survives the convolution where it was.
            const float ggxPatch = atlasAt(atlas, mapRoughnessUv(toSphericalUv(patch), 1.0f));
            const float ggxMirrored = atlasAt(atlas, mapRoughnessUv(toSphericalUv(mirrored), 1.0f));
            check(ggxPatch > 2.0f && ggxMirrored < 1.0f, "GGX rect: bright at the patch (" + number(ggxPatch) +
                "), dark at its mirror (" + number(ggxMirrored) + ")");

            // The Lambert rect: the mirror direction is 120 degrees from the patch, so
            // its hemisphere does not see the patch at all.
            const float lambertPatch = atlasAt(atlas, mapAmbientUv(toSphericalUv(patch)));
            const float lambertMirrored = atlasAt(atlas, mapAmbientUv(toSphericalUv(mirrored)));
            check(lambertPatch > 0.3f && lambertPatch > 4.0f * lambertMirrored, "Lambert rect: the irradiance "
                "facing the patch (" + number(lambertPatch) + ") is far above that facing its mirror (" +
                number(lambertMirrored) + ")");
        }

        // ── The skybox cube built from the same equirect ─────────────────────
        // The sky samples it along the view direction with X negated, the reflections
        // read the atlas with X negated: both agree only if the cube holds the patch
        // along `patch` as the atlas does.
        constexpr uint32_t kFace = 64;
        std::unique_ptr<Texture> skybox(EnvLighting::generateSkyboxCubemap(device.get(), source.get(),
            static_cast<int>(kFace)));
        check(skybox != nullptr, "the skybox cube is built");
        if (skybox) {
            auto cubeAt = [&](const Dir& d, float& value) {
                uint32_t face = 0;
                Uv uv{};
                cubeFaceUv(d, face, uv);
                std::vector<uint8_t> texels;
                if (!skybox->read(texels, 0, 0, kFace, kFace, 0, face)) {
                    return false;
                }
                const uint32_t x = std::min(static_cast<uint32_t>(uv.u * kFace), kFace - 1);
                const uint32_t y = std::min(static_cast<uint32_t>(uv.v * kFace), kFace - 1);
                value = decodeRgbp(&texels[(static_cast<size_t>(y) * kFace + x) * 4]);
                return true;
            };
            float cubePatch = 0.0f;
            float cubeMirrored = 0.0f;
            const bool readCube = cubeAt(patch, cubePatch) && cubeAt(mirrored, cubeMirrored);
            check(readCube, "the skybox faces read back");
            check(readCube && cubePatch > 5.0f && cubeMirrored < 1.0f, "skybox: the patch is along its own "
                "direction (" + number(cubePatch) + "), not its mirror (" + number(cubeMirrored) + ")");
        }
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("env atlas orientation");
}
