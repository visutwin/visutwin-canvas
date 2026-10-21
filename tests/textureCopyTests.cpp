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
// texture copied onto itself) must copy nothing. An array texture's layers read, copy and
// render one at a time: a layer's own pattern back, a copy into one layer only, and a clear
// rendered into one layer through a render target, with that layer's mips generated. A volume
// reads, copies and renders by depth slice the same way: one slice or a run of them, packed
// slice after slice, its mips (which halve the depth too) uploaded, generated after the upload
// and generated after rendering into its slices. The volume checks follow upstream's layered
// texture test (test/texture-copy-read-layered), each texel unique to its slice.
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
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
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
        const bool mipmaps, const bool cubemap = false, const PixelFormat format = PixelFormat::PIXELFORMAT_RGBA8,
        const uint32_t arrayLength = 0)
    {
        TextureOptions options;
        options.arrayLength = arrayLength;
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

    std::unique_ptr<Texture> makeVolume(GraphicsDevice* device, const char* name, const uint32_t size,
        const uint32_t depth, const bool mipmaps)
    {
        TextureOptions options;
        options.name = name;
        options.width = size;
        options.height = size;
        options.depth = depth;
        options.volume = true;
        options.mipmaps = mipmaps;
        options.minFilter = mipmaps ? FilterMode::FILTER_LINEAR_MIPMAP_LINEAR : FilterMode::FILTER_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;
        return std::make_unique<Texture>(device, options);
    }

    // One slice whose every texel is unique to its position, its slice and the seed.
    std::vector<uint8_t> slicePixels(const uint32_t size, const uint32_t slice, const uint32_t seed)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                uint8_t* texel = pixels.data() + (static_cast<size_t>(y) * size + x) * 4;
                texel[0] = static_cast<uint8_t>(x * 16 + seed);
                texel[1] = static_cast<uint8_t>(y * 16 + seed);
                texel[2] = static_cast<uint8_t>(slice * 24 + seed);
                texel[3] = static_cast<uint8_t>(255 - seed);
            }
        }
        return pixels;
    }

    // Slices [first, first + count) of a volume of slicePixels, packed one after another.
    std::vector<uint8_t> volumePixels(const uint32_t size, const uint32_t first, const uint32_t count,
        const uint32_t seed)
    {
        std::vector<uint8_t> pixels;
        for (uint32_t slice = first; slice < first + count; ++slice) {
            const auto one = slicePixels(size, slice, seed);
            pixels.insert(pixels.end(), one.begin(), one.end());
        }
        return pixels;
    }

    // A flat colour per slice, chosen so the averages of neighbouring slices are whole numbers.
    Rgba sliceColor(const uint32_t slice)
    {
        return {static_cast<uint8_t>(slice * 32), static_cast<uint8_t>(255 - slice * 32), 128, 255};
    }

    // A slice of mip `level`: the average of the 2^level slices of mip 0 it is filtered from.
    Rgba mipSliceColor(const uint32_t level, const uint32_t slice)
    {
        const uint32_t count = 1u << level;
        const uint32_t first = slice * count;
        const uint32_t r = (first + (first + count - 1)) * 16;
        return {static_cast<uint8_t>(r), static_cast<uint8_t>(255 - r), 128, 255};
    }

    // Flat slices of the given colours, packed one after another.
    std::vector<uint8_t> flatSlices(const uint32_t size, const std::vector<Rgba>& colors)
    {
        std::vector<uint8_t> pixels;
        for (const Rgba& color : colors) {
            const auto one = solid(size, color);
            pixels.insert(pixels.end(), one.begin(), one.end());
        }
        return pixels;
    }

    // Equal within one count per channel: the rounding of a filtered mip.
    bool close(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected)
    {
        if (actual.size() != expected.size()) {
            return false;
        }
        for (size_t i = 0; i < actual.size(); ++i) {
            if (std::abs(static_cast<int>(actual[i]) - static_cast<int>(expected[i])) > 1) {
                return false;
            }
        }
        return true;
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

        // ── Array layers ─────────────────────────────────────────────────────
        {
            constexpr uint32_t kLayers = 4;
            constexpr uint32_t kArray = 16;
            const std::array<Rgba, kLayers> layerColors{kRed, kGreen, kBlue, kYellow};
            const auto layerPattern = [&](const uint32_t layer) {
                return layer == 2 ? quadrants(kArray, {kRed, kGreen, kBlue, kYellow}) : solid(kArray, layerColors[layer]);
            };
            auto source = makeTexture(device.get(), "array-source", kArray, false, false,
                PixelFormat::PIXELFORMAT_RGBA8, kLayers);
            auto dest = makeTexture(device.get(), "array-dest", kArray, false, false, PixelFormat::PIXELFORMAT_RGBA8,
                kLayers);
            for (uint32_t layer = 0; layer < kLayers; ++layer) {
                fill(*source, layerPattern(layer), layer);
                fill(*dest, solid(kArray, kGrey), layer);
            }
            source->upload();
            dest->upload();
            int readBack = 0;
            for (uint32_t layer = 0; layer < kLayers; ++layer) {
                std::vector<uint8_t> actual;
                if (source->read(actual, 0, 0, 0, 0, 0, layer) && actual == layerPattern(layer)) {
                    ++readBack;
                }
            }
            check(readBack == static_cast<int>(kLayers), "every layer reads back its own pattern");
            std::vector<uint8_t> none;
            check(!source->read(none, 0, 0, 0, 0, 0, kLayers), "a layer past the last is refused");

            Texture::CopyOptions layer;
            layer.face = 2;
            check(dest->copy(*source, layer), "layer 2 copies");
            std::vector<uint8_t> copied;
            std::vector<uint8_t> untouched;
            check(dest->read(copied, 0, 0, 0, 0, 0, 2) && copied == layerPattern(2), "layer 2 holds the source's layer 2");
            check(dest->read(untouched, 0, 0, 0, 0, 0, 1) && untouched == solid(kArray, kGrey), "layer 1 is untouched");

            std::vector<uint8_t> async;
            device->frameStart();
            check(source->readAsync([&](const uint8_t* data, const size_t size) { async.assign(data, data + size); },
                0, 0, 0, 0, 0, 2), "a read of one layer is queued");
            device->frameEnd();
            for (int frame = 0; frame < 8 && async.empty(); ++frame) {
                device->frameStart();
                device->frameEnd();
            }
            check(async == layerPattern(2), "and delivers that layer");
        }

        // ── Rendering into one array layer ───────────────────────────────────
        {
            constexpr uint32_t kLayers = 3;
            constexpr uint32_t kArray = 16;
            constexpr uint32_t kTarget = 1;
            auto array = makeTexture(device.get(), "array-target", kArray, true, false, PixelFormat::PIXELFORMAT_RGBA8,
                kLayers);
            for (uint32_t layer = 0; layer < kLayers; ++layer) {
                fill(*array, solid(kArray, kGrey), layer);
            }
            array->upload();

            RenderTargetOptions targetOptions;
            targetOptions.graphicsDevice = device.get();
            targetOptions.colorBuffer = array.get();
            targetOptions.face = static_cast<int>(kTarget);
            targetOptions.name = "array-layer-target";
            const auto target = device->createRenderTarget(targetOptions);
            // the pass borrows the device, which outlives it
            const std::shared_ptr<GraphicsDevice> shared(device.get(), [](GraphicsDevice*) {});
            auto pass = std::make_shared<RenderPass>(shared);
            pass->init(target);
            const Color clear(40.0f / 255.0f, 80.0f / 255.0f, 230.0f / 255.0f, 1.0f);
            pass->setClearColor(&clear);
            pass->colorOps()->genMipmaps = true;
            device->frameStart();
            pass->render();
            device->frameEnd();

            std::vector<uint8_t> rendered;
            std::vector<uint8_t> renderedMip;
            std::vector<uint8_t> other;
            check(array->read(rendered, 0, 0, 0, 0, 0, kTarget) && rendered == solid(kArray, kBlue),
                "a render target over layer 1 clears that layer");
            check(array->read(renderedMip, 0, 0, 0, 0, 1, kTarget) && renderedMip == solid(kArray / 2, kBlue),
                "and generates its mips");
            check(array->read(other, 0, 0, 0, 0, 0, 0) && other == solid(kArray, kGrey), "layer 0 is untouched");
            check(array->read(other, 0, 0, 0, 0, 0, 2) && other == solid(kArray, kGrey), "layer 2 is untouched");
        }

        // The device the render passes borrow; it outlives them.
        const std::shared_ptr<GraphicsDevice> sharedDevice(device.get(), [](GraphicsDevice*) {});
        const auto clearSlice = [&](const std::shared_ptr<RenderTarget>& target, const Rgba& color,
            const bool genMipmaps) {
            auto pass = std::make_shared<RenderPass>(sharedDevice);
            pass->init(target);
            const Color clear(color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f);
            pass->setClearColor(&clear);
            pass->colorOps()->genMipmaps = genMipmaps;
            device->frameStart();
            pass->render();
            device->frameEnd();
        };
        const auto sliceTarget = [&](Texture* volume, const uint32_t slice, const bool depth = false,
            const int samples = 1) {
            RenderTargetOptions options;
            options.graphicsDevice = device.get();
            options.colorBuffer = volume;
            options.face = static_cast<int>(slice);
            options.depth = depth;
            options.samples = samples;
            options.name = "volume-slice";
            return device->createRenderTarget(options);
        };

        // ── Volume depth slices ──────────────────────────────────────────────
        {
            constexpr uint32_t kVolume = 8;
            constexpr uint32_t kDepth = 6;
            auto source = makeVolume(device.get(), "volume-source", kVolume, kDepth, false);
            const auto sourcePixels = volumePixels(kVolume, 0, kDepth, 3);
            fill(*source, sourcePixels);
            source->upload();
            int slicesRead = 0;
            for (uint32_t slice = 0; slice < kDepth; ++slice) {
                std::vector<uint8_t> actual;
                if (source->read(actual, 0, 0, 0, 0, 0, slice) && actual == slicePixels(kVolume, slice, 3)) {
                    ++slicesRead;
                }
            }
            check(slicesRead == static_cast<int>(kDepth), "volume: every slice reads back its own texels");
            std::vector<uint8_t> all;
            check(source->read(all, 0, 0, 0, 0, 0, 0, 0) && all == sourcePixels,
                "volume: a slice count of 0 reads every slice, packed");
            std::vector<uint8_t> run;
            check(source->read(run, 0, 0, 0, 0, 0, 2, 3) && run == volumePixels(kVolume, 2, 3, 3),
                "volume: slices 2-4 read as a run");
            std::vector<uint8_t> none;
            check(!source->read(none, 0, 0, 0, 0, 0, kDepth), "volume: a slice past the last is refused");
            check(!source->read(none, 0, 0, 0, 0, 0, 4, 3), "volume: a run leaving the volume is refused");

            const auto destPixels = volumePixels(kVolume, 0, kDepth, 9);
            auto dest = makeVolume(device.get(), "volume-dest", kVolume, kDepth, false);
            fill(*dest, destPixels);
            dest->upload();
            Texture::CopyOptions slice4;
            slice4.face = 4;
            check(dest->copy(*source, slice4), "volume: slice 4 copies");
            std::vector<uint8_t> copied;
            check(dest->read(copied, 0, 0, 0, 0, 0, 4) && copied == slicePixels(kVolume, 4, 3),
                "volume: slice 4 holds the source's slice 4");
            check(dest->read(copied, 0, 0, 0, 0, 0, 3) && copied == slicePixels(kVolume, 3, 9),
                "volume: slice 3 is untouched");

            auto destAll = makeVolume(device.get(), "volume-dest-all", kVolume, kDepth, false);
            fill(*destAll, destPixels);
            destAll->upload();
            Texture::CopyOptions everySlice;
            everySlice.sliceCount = 0;
            check(destAll->copy(*source, everySlice) && destAll->read(copied, 0, 0, 0, 0, 0, 0, 0) &&
                copied == sourcePixels, "volume: every slice copies");

            // the top-left quarter of every slice into the bottom-right quarter
            auto destRegion = makeVolume(device.get(), "volume-dest-region", kVolume, kDepth, false);
            fill(*destRegion, destPixels);
            destRegion->upload();
            Texture::CopyOptions region;
            region.sliceCount = 0;
            region.width = kVolume / 2;
            region.height = kVolume / 2;
            region.destX = kVolume / 2;
            region.destY = kVolume / 2;
            bool regionMatches = destRegion->copy(*source, region) && destRegion->read(copied, 0, 0, 0, 0, 0, 0, 0);
            for (uint32_t slice = 0; regionMatches && slice < kDepth; ++slice) {
                for (uint32_t y = 0; y < kVolume; ++y) {
                    for (uint32_t x = 0; x < kVolume; ++x) {
                        const bool moved = x >= kVolume / 2 && y >= kVolume / 2;
                        const auto& expectedSlice = moved ? sourcePixels : destPixels;
                        const size_t sourceTexel = (static_cast<size_t>(slice) * kVolume * kVolume +
                            (moved ? (y - kVolume / 2) * kVolume + (x - kVolume / 2) : y * kVolume + x)) * 4;
                        const size_t destTexel = (static_cast<size_t>(slice) * kVolume * kVolume + y * kVolume + x) * 4;
                        for (int c = 0; c < 4; ++c) {
                            regionMatches &= copied[destTexel + c] == expectedSlice[sourceTexel + c];
                        }
                    }
                }
            }
            check(regionMatches, "volume: a sub-rectangle of every slice copies to an offset");
            auto flat = makeTexture(device.get(), "volume-refuse-2d", kVolume, false);
            flat->upload();
            check(!flat->copy(*source), "volume: a copy from a volume to a 2D texture is refused");

            std::vector<uint8_t> async;
            device->frameStart();
            check(source->readAsync([&](const uint8_t* data, const size_t size) { async.assign(data, data + size); },
                0, 0, 0, 0, 0, 1, 2), "volume: a read of two slices is queued");
            device->frameEnd();
            for (int frame = 0; frame < 8 && async.empty(); ++frame) {
                device->frameStart();
                device->frameEnd();
            }
            check(async == volumePixels(kVolume, 1, 2, 3), "volume: and delivers those slices");
        }

        // ── Volume mip levels ────────────────────────────────────────────────
        {
            // 8x8x6 has 4 levels: 8x8x6, 4x4x3, 2x2x1 and 1x1x1
            auto volume = makeVolume(device.get(), "volume-mips", 8, 6, true);
            constexpr std::array<std::array<uint32_t, 2>, 4> kLevels{{{8, 6}, {4, 3}, {2, 1}, {1, 1}}};
            std::vector<std::vector<uint8_t>> levels;
            for (uint32_t level = 0; level < kLevels.size(); ++level) {
                levels.push_back(volumePixels(kLevels[level][0], 0, kLevels[level][1], 4 + level));
                volume->setLevelData(level, levels.back().data(), levels.back().size());
            }
            volume->upload();
            check(volume->getNumLevels() == 4, "volume mips: 4 levels, the depth halving with the rest");
            std::vector<uint8_t> actual;
            check(volume->read(actual, 0, 0, 0, 0, 1, 0, 0) && actual == levels[1],
                "volume mips: every slice of mip 1 reads back");
            check(volume->read(actual, 0, 0, 0, 0, 1, 2) && actual == slicePixels(4, 2, 5),
                "volume mips: slice 2 of mip 1 reads back");
            check(!volume->read(actual, 0, 0, 0, 0, 2, 1), "volume mips: mip 2 has one slice");

            auto dest = makeVolume(device.get(), "volume-mips-dest", 4, 3, false);
            fill(*dest, volumePixels(4, 0, 3, 11));
            dest->upload();
            Texture::CopyOptions mip1;
            mip1.sourceMipLevel = 1;
            mip1.sliceCount = 0;
            check(dest->copy(*volume, mip1) && dest->read(actual, 0, 0, 0, 0, 0, 0, 0) && actual == levels[1],
                "volume mips: every slice of mip 1 copies into a smaller volume's mip 0");
        }

        // ── Rendering into volume depth slices ───────────────────────────────
        {
            constexpr uint32_t kVolume = 4;
            constexpr uint32_t kDepth = 4;
            auto volume = makeVolume(device.get(), "render-volume", kVolume, kDepth, false);
            volume->upload();
            std::vector<std::shared_ptr<RenderTarget>> targets;
            std::vector<Rgba> colors;
            for (uint32_t slice = 0; slice < kDepth; ++slice) {
                targets.push_back(sliceTarget(volume.get(), slice));
                colors.push_back(sliceColor(slice));
                clearSlice(targets.back(), colors.back(), false);
            }
            std::vector<uint8_t> actual;
            check(volume->read(actual, 0, 0, 0, 0, 0, 0, 0) && actual == flatSlices(kVolume, colors),
                "render: every slice holds its own colour");
            clearSlice(targets[2], Rgba{10, 20, 30, 255}, false);
            check(volume->read(actual, 0, 0, 0, 0, 0, 2) && actual == solid(kVolume, Rgba{10, 20, 30, 255}),
                "render: one slice renders");
            check(volume->read(actual, 0, 0, 0, 0, 0, 1) && actual == solid(kVolume, sliceColor(1)),
                "render: and slice 1 is untouched");
            targets.clear();

            const auto depthTarget = sliceTarget(volume.get(), 3, true);
            clearSlice(depthTarget, Rgba{40, 50, 60, 255}, false);
            check(volume->read(actual, 0, 0, 0, 0, 0, 3) && actual == solid(kVolume, Rgba{40, 50, 60, 255}),
                "render: a slice with a depth buffer renders");
            const auto msaaTarget = sliceTarget(volume.get(), 0, false, 4);
            check(msaaTarget->samples() == 1, "render: multisampling a slice is ignored");
            clearSlice(msaaTarget, Rgba{70, 80, 90, 255}, false);
            check(volume->read(actual, 0, 0, 0, 0, 0, 0) && actual == solid(kVolume, Rgba{70, 80, 90, 255}),
                "render: a slice asked for multisampling renders");
        }

        // ── Volume mips generated after rendering and after upload ───────────
        {
            constexpr uint32_t kVolume = 8;
            constexpr uint32_t kDepth = 8;
            const auto expectedMip = [&](const uint32_t level) {
                std::vector<Rgba> colors;
                for (uint32_t slice = 0; slice < (kDepth >> level); ++slice) {
                    colors.push_back(mipSliceColor(level, slice));
                }
                return flatSlices(kVolume >> level, colors);
            };

            // every slice rendered, the mips generated once, after the last
            auto rendered = makeVolume(device.get(), "render-volume-mips", kVolume, kDepth, true);
            rendered->upload();
            for (uint32_t slice = 0; slice < kDepth; ++slice) {
                clearSlice(sliceTarget(rendered.get(), slice), sliceColor(slice), slice == kDepth - 1);
            }
            std::vector<uint8_t> actual;
            int renderedMips = 0;
            for (uint32_t level = 1; level <= 3; ++level) {
                renderedMips += rendered->read(actual, 0, 0, 0, 0, level, 0, 0) && close(actual, expectedMip(level));
            }
            check(renderedMips == 3, "render mips: mips 1-3 filter neighbouring slices together");

            std::vector<Rgba> colors;
            for (uint32_t slice = 0; slice < kDepth; ++slice) {
                colors.push_back(sliceColor(slice));
            }
            auto uploaded = makeVolume(device.get(), "upload-volume-mips", kVolume, kDepth, true);
            fill(*uploaded, flatSlices(kVolume, colors));
            uploaded->upload();
            int uploadedMips = 0;
            for (uint32_t level = 1; level <= 3; ++level) {
                uploadedMips += uploaded->read(actual, 0, 0, 0, 0, level, 0, 0) && close(actual, expectedMip(level));
            }
            check(uploadedMips == 3, "upload mips: mips 1-3 generated from the uploaded level");
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
