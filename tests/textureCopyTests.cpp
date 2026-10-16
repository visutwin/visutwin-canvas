// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Texture::copy on a real device of whichever backend this build has: a whole level, a
// source mip into a destination's level 0, a sub-rectangle to an offset over a destination
// that already holds data, and one cube face. Every result is read back with Texture::read
// and compared texel for texel, so a copy that lands at the wrong place, mip or face, or not
// at all, fails. The refusals (another format, a region leaving the level, a missing face, a
// texture copied onto itself) must copy nothing.
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/texture.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    using Rgba = std::array<uint8_t, 4>;

    constexpr Rgba kRed{220, 40, 40, 255};
    constexpr Rgba kGreen{40, 200, 40, 255};
    constexpr Rgba kBlue{40, 80, 230, 255};
    constexpr Rgba kYellow{230, 220, 40, 255};
    constexpr Rgba kGrey{60, 60, 60, 255};

    std::unique_ptr<Texture> makeTexture(GraphicsDevice* device, const char* name, const uint32_t size,
        const bool mipmaps, const bool cubemap = false, const PixelFormat format = PixelFormat::PIXELFORMAT_RGBA8)
    {
        TextureOptions options;
        options.name = name;
        options.width = size;
        options.height = size;
        options.format = format;
        options.mipmaps = mipmaps;
        options.cubemap = cubemap;
        options.minFilter = mipmaps ? FilterMode::FILTER_LINEAR_MIPMAP_LINEAR : FilterMode::FILTER_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;
        return std::make_unique<Texture>(device, options);
    }

    // Four quadrants: top left, top right, bottom left, bottom right.
    std::vector<uint8_t> quadrants(const uint32_t size, const std::array<Rgba, 4>& colors)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
        const uint32_t half = size / 2;
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const Rgba& c = colors[(y < half ? 0 : 2) + (x < half ? 0 : 1)];
                std::copy(c.begin(), c.end(), pixels.begin() + (static_cast<size_t>(y) * size + x) * 4);
            }
        }
        return pixels;
    }

    std::vector<uint8_t> solid(const uint32_t size, const Rgba& color)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            std::copy(color.begin(), color.end(), pixels.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return pixels;
    }

    void fill(Texture& texture, const std::vector<uint8_t>& pixels, const uint32_t face = 0)
    {
        texture.setLevelData(0, pixels.data(), pixels.size(), face);
    }
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
    SDL_Window* window = SDL_CreateWindow("texture-copy", 64, 64, flags);
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

        constexpr uint32_t kSize = 64;
        const std::vector<uint8_t> pattern = quadrants(kSize, {kRed, kGreen, kBlue, kYellow});

        // ── A whole level ────────────────────────────────────────────────────
        {
            auto source = makeTexture(device.get(), "copy-source", kSize, false);
            fill(*source, pattern);
            source->upload();
            auto dest = makeTexture(device.get(), "copy-dest", kSize, false);
            dest->upload();
            check(dest->copy(*source), "a whole level copies");
            std::vector<uint8_t> actual;
            check(dest->read(actual) && actual == pattern, "the copy holds the source texel for texel");
        }

        // ── A source mip into the destination's level 0 ──────────────────────
        {
            auto source = makeTexture(device.get(), "copy-mips", kSize, true);
            fill(*source, pattern);
            source->upload();
            std::vector<uint8_t> expected;
            check(source->read(expected, 0, 0, 0, 0, 1), "the source's mip 1 reads back");
            auto dest = makeTexture(device.get(), "copy-mip-dest", kSize / 2, false);
            dest->upload();
            Texture::CopyOptions mip;
            mip.sourceMipLevel = 1;
            check(dest->copy(*source, mip), "mip 1 copies into level 0");
            std::vector<uint8_t> actual;
            check(dest->read(actual) && actual == expected && !actual.empty(),
                "the destination holds the source's mip 1");
        }

        // ── A sub-rectangle to an offset, over data already there ────────────
        {
            auto source = makeTexture(device.get(), "copy-rect-source", kSize, false);
            fill(*source, pattern);
            source->upload();
            auto dest = makeTexture(device.get(), "copy-rect-dest", kSize, false);
            fill(*dest, solid(kSize, kGrey));
            dest->upload();
            Texture::CopyOptions rect;
            rect.width = kSize / 2;
            rect.height = kSize / 2;
            rect.destX = kSize / 2;
            rect.destY = kSize / 2;
            check(dest->copy(*source, rect), "the top-left quarter copies to the bottom right");
            std::vector<uint8_t> expected = solid(kSize, kGrey);
            for (uint32_t y = kSize / 2; y < kSize; ++y) {
                for (uint32_t x = kSize / 2; x < kSize; ++x) {
                    std::copy(kRed.begin(), kRed.end(), expected.begin() + (static_cast<size_t>(y) * kSize + x) * 4);
                }
            }
            std::vector<uint8_t> actual;
            check(dest->read(actual) && actual == expected,
                "red lands in the bottom-right quarter and the grey around it is kept");
        }

        // ── One cube face ────────────────────────────────────────────────────
        {
            constexpr uint32_t kFace = 3;
            constexpr uint32_t kCube = 16;
            auto source = makeTexture(device.get(), "copy-cube-source", kCube, false, true);
            auto dest = makeTexture(device.get(), "copy-cube-dest", kCube, false, true);
            for (uint32_t face = 0; face < 6; ++face) {
                fill(*source, face == kFace ? quadrants(kCube, {kRed, kGreen, kBlue, kYellow}) : solid(kCube, kBlue),
                    face);
                fill(*dest, solid(kCube, kGrey), face);
            }
            source->upload();
            dest->upload();
            Texture::CopyOptions face;
            face.face = kFace;
            check(dest->copy(*source, face), "cube face 3 copies");
            std::vector<uint8_t> copied;
            std::vector<uint8_t> untouched;
            check(dest->read(copied, 0, 0, 0, 0, 0, kFace) &&
                copied == quadrants(kCube, {kRed, kGreen, kBlue, kYellow}), "face 3 holds the source's face 3");
            check(dest->read(untouched, 0, 0, 0, 0, 0, 2) && untouched == solid(kCube, kGrey),
                "face 2 is untouched");
        }

        // ── Refusals ─────────────────────────────────────────────────────────
        {
            auto source = makeTexture(device.get(), "refuse-source", kSize, false);
            fill(*source, pattern);
            source->upload();
            auto other = makeTexture(device.get(), "refuse-format", kSize, false, false,
                PixelFormat::PIXELFORMAT_RGBA16F);
            other->upload();
            check(!other->copy(*source), "a copy between formats is refused");
            auto dest = makeTexture(device.get(), "refuse-dest", kSize, false);
            fill(*dest, solid(kSize, kGrey));
            dest->upload();
            Texture::CopyOptions outside;
            outside.destX = kSize / 2;
            check(!dest->copy(*source, outside), "a region leaving the destination is refused");
            Texture::CopyOptions face;
            face.face = 1;
            check(!dest->copy(*source, face), "a face a 2D texture does not have is refused");
            check(!dest->copy(*dest), "a texture is not copied onto itself");
            std::vector<uint8_t> actual;
            check(dest->read(actual) && actual == solid(kSize, kGrey), "a refused copy writes nothing");
        }

        // ── A read that does not block (Texture::readAsync) ──────────────────
        {
            auto source = makeTexture(device.get(), "read-async", kSize, false);
            fill(*source, pattern);
            source->upload();
            std::vector<uint8_t> whole;
            std::vector<uint8_t> quarter;
            int calls = 0;
            bool dropped = false;
            device->frameStart();
            check(source->readAsync([&](const uint8_t* data, const size_t size) {
                whole.assign(data, data + size);
                ++calls;
            }), "a read is queued inside a frame");
            check(source->readAsync([&](const uint8_t* data, const size_t size) {
                quarter.assign(data, data + size);
                ++calls;
            }, kSize / 2, 0, kSize / 2, kSize / 2), "and a sub-rectangle");
            check(!source->readAsync([&](const uint8_t*, size_t) {}, kSize, 0, 1, 1),
                "a region outside the level is refused");
            {
                auto shortLived = makeTexture(device.get(), "read-async-gone", kSize, false);
                fill(*shortLived, pattern);
                shortLived->upload();
                shortLived->readAsync([&](const uint8_t*, size_t) { dropped = true; });
            }
            check(calls == 0, "nothing arrives inside the frame that queued it");
            device->frameEnd();
            for (int frame = 0; frame < 8 && calls < 2; ++frame) {
                device->frameStart();
                device->frameEnd();
            }
            check(calls == 2, "both reads arrive at a later frame start");
            check(whole == pattern, "the whole level texel for texel");
            std::vector<uint8_t> expected;
            for (uint32_t y = 0; y < kSize / 2; ++y) {
                for (uint32_t x = kSize / 2; x < kSize; ++x) {
                    expected.insert(expected.end(), kGreen.begin(), kGreen.end());
                }
            }
            check(quarter == expected, "the top-right quarter, tightly packed");
            // Metal retains the GPU texture until the copy is encoded, so the read still arrives;
            // Vulkan cancels it with the texture.
            check(backend == Backend::Metal || !dropped, "a texture gone before its frame ends cancels its read");
        }
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("texture copy");
}
