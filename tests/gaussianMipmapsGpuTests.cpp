// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
// The Gaussian mipmap chain (GaussianMipmapGenerator, the Slang program `mipmap-gaussian`) on a
// real device of this build's backend. Each level is rendered from the level above into a
// scratch texture and copied back into the chain; the copy must read only the corner of the
// scratch the level was rendered into. A horizontal ramp (value = u at every texel centre) is
// the probe: a symmetric blur of a linear ramp is the same ramp, so away from the edges, where
// the taps are clamped, every level must read u = (x + 0.5) / width again. A copy that read the
// whole scratch would stretch the level and show the level above around it.
//
// Two cases: a texture larger than any served before (the scratch is its level 1's size), and a
// smaller texture through the SAME generator afterwards (the scratch kept the larger size, so
// even level 1 fills only a corner of it).
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
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/math/color.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/gaussianMipmaps.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // An RGBA32F texture whose top level holds the ramp u in red, asking for the Gaussian chain.
    std::shared_ptr<Texture> rampTexture(GraphicsDevice* device, const uint32_t size)
    {
        TextureOptions options;
        options.name = "gaussian-ramp";
        options.width = size;
        options.height = size;
        options.format = PixelFormat::PIXELFORMAT_RGBA32F;
        options.mipmaps = true;
        options.mipmapFilter = MipmapFilter::MIPMAPFILTER_GAUSSIAN;
        auto texture = std::make_shared<Texture>(device, options);
        texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        std::vector<float> texels(static_cast<size_t>(size) * size * 4);
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                float* texel = &texels[(static_cast<size_t>(y) * size + x) * 4];
                texel[0] = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                texel[1] = 0.0f;
                texel[2] = 0.0f;
                texel[3] = 1.0f;
            }
        }
        texture->setLevelData(0, reinterpret_cast<const uint8_t*>(texels.data()), texels.size() * sizeof(float));
        texture->upload();
        return texture;
    }

    // Every level below the top must read the ramp again, away from the edges the taps clamp at.
    void checkLevels(Texture* texture, const std::string& what)
    {
        for (uint32_t level = 1; level < texture->getNumLevels(); ++level) {
            const uint32_t width = std::max(texture->width() >> level, 1u);
            const uint32_t height = std::max(texture->height() >> level, 1u);
            // the taps reach 2 texels of the level above: 1 texel of this level from each edge
            if (width < 6) {
                break;
            }
            std::vector<uint8_t> bytes;
            const bool read = texture->read(bytes, 0, 0, 0, 0, level);
            std::vector<float> texels(static_cast<size_t>(width) * height * 4);
            if (read && bytes.size() == texels.size() * sizeof(float)) {
                std::memcpy(texels.data(), bytes.data(), bytes.size());
            }
            float worst = 0.0f;
            uint32_t worstX = 0;
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 2; x + 2 < width; ++x) {
                    const float expected = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
                    const float error = std::abs(texels[(static_cast<size_t>(y) * width + x) * 4] - expected);
                    if (error > worst) {
                        worst = error;
                        worstX = x;
                    }
                }
            }
            check(read && worst < 1e-3f, what + ": level " + std::to_string(level) + " (" + std::to_string(width) +
                ") reads the ramp, worst error " + std::to_string(worst) + " at x " + std::to_string(worstX));
        }
    }
}

int main()
{
    std::cout << std::unitbuf;
    const Backend backend = defaultBackend();
    if (backend == Backend::Metal) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }
    const SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | (backend == Backend::Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_METAL);
    SDL_Window* window = SDL_CreateWindow("gaussian-mipmaps", 64, 64, flags);
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
        std::shared_ptr<GraphicsDevice> device = createGraphicsDevice(options);
        check(device != nullptr, std::string("a ") + backendName(backend) + " device is created");
        if (!device) {
            return 1;
        }
        // The generator borrows the device, which outlives it.
        const std::shared_ptr<GraphicsDevice> shared(device.get(), [](GraphicsDevice*) {});
        GaussianMipmapGenerator generator(shared);

        std::cout << "\na 128x128 ramp: the scratch is its level 1's size\n";
        auto large = rampTexture(device.get(), 128);
        device->frameStart();
        check(generator.generate(large.get()), "the chain is rendered");
        device->frameEnd();
        checkLevels(large.get(), "128");

        std::cout << "\na 64x64 ramp through the same generator: the scratch is larger than its level 1\n";
        auto small = rampTexture(device.get(), 64);
        device->frameStart();
        check(generator.generate(small.get()), "the chain is rendered");
        device->frameEnd();
        checkLevels(small.get(), "64");
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("gaussian mipmaps gpu");
}
