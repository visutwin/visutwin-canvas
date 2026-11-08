// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
// Order-independent transparency's arithmetic on a real device of this build's backend: the
// accumulation blend state (RenderPassOit::accumulationBlendState) over its two attachments,
// RGBA16F and RGBA8, and the composite program (oit-composite) with its blend state over a
// background. Two layers are drawn as fullscreen quads by a small program writing what the
// forward program's accumulation entry writes (colour * alpha * weight and alpha * weight, the
// coverage alpha):
//   - the result is the weighted average blended by the coverage, as the closed form says:
//     red at alpha 0.5, weight 2 and blue at alpha 0.6, weight 1 average to (0.625, 0, 0.375),
//     cover 1 - 0.5 x 0.4 = 0.8, and over a green background give (0.5, 0.2, 0.3);
//   - the two layers drawn the other way round give the same pixel;
//   - a pixel no layer covers leaves the background as it was.
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

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/math/color.h"
#include "core/math/vector4.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/quadRender.h"
#include "scene/renderer/renderPassOit.h"
#include "scene/shader-lib/slangShaders.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr uint32_t kSize = 16;

    // One layer of the accumulation, as forwardFragmentOit writes it; the layer covers the left
    // half of the target only, so the right half is a pixel nothing covers.
    constexpr const char* kLayerProgram = R"(
import vtquad;
#include "bindings.slang"

struct LayerParams { float4 color; float4 alphaWeight; };
VT_DECLARE_QUAD_UNIFORMS(LayerParams, layer)

struct LayerOut
{
    float4 accumulation : SV_Target0;
    float4 coverage : SV_Target1;
};

[shader("vertex")]
QuadVarying layerVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
LayerOut layerFragment(QuadVarying input)
{
    if (input.uv.x > 0.5) {
        discard;
    }
    const float alpha = layer.alphaWeight.x;
    const float weight = layer.alphaWeight.y;
    LayerOut result;
    result.accumulation = float4(layer.color.rgb * alpha, alpha) * weight;
    result.coverage = float4(alpha, alpha, alpha, alpha);
    return result;
}
)";

    struct LayerParams
    {
        float color[4];
        float alphaWeight[4];
    };

    std::unique_ptr<Texture> colorTexture(GraphicsDevice* device, const char* name, const PixelFormat format)
    {
        TextureOptions options;
        options.name = name;
        options.width = kSize;
        options.height = kSize;
        options.format = format;
        options.mipmaps = false;
        return std::make_unique<Texture>(device, options);
    }

    struct Pixel
    {
        int r = -1;
        int g = -1;
        int b = -1;
    };

    Pixel readPixel(Texture* texture, const uint32_t x, const uint32_t y)
    {
        std::vector<uint8_t> pixels;
        if (!texture->read(pixels) || pixels.size() != static_cast<size_t>(kSize) * kSize * 4) {
            return {};
        }
        const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
        return {pixels[i], pixels[i + 1], pixels[i + 2]};
    }

    bool near(const Pixel& pixel, const float r, const float g, const float b)
    {
        const auto close = [](const int value, const float expected) {
            return std::abs(static_cast<float>(value) - expected * 255.0f) <= 1.5f;
        };
        return close(pixel.r, r) && close(pixel.g, g) && close(pixel.b, b);
    }

    std::string describe(const Pixel& pixel)
    {
        return "(" + std::to_string(pixel.r) + ", " + std::to_string(pixel.g) + ", " + std::to_string(pixel.b) + ")";
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
    SDL_Window* window = SDL_CreateWindow("oit", 64, 64, flags);
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
        const std::shared_ptr<GraphicsDevice> shared(device.get(), [](GraphicsDevice*) {});
        const auto layerShader = getOrCreateSlangShaderFromSource(device.get(), "oit-test-layer", kLayerProgram);
        const auto compositeShader = getOrCreateSlangShader(device.get(), "oit-composite");
        check(layerShader != nullptr && compositeShader != nullptr, "the layer and composite programs compile");
        if (!layerShader || !compositeShader) {
            return 1;
        }

        auto accumulation = colorTexture(device.get(), "oit-accumulation", PixelFormat::PIXELFORMAT_RGBA16F);
        auto coverage = colorTexture(device.get(), "oit-coverage", PixelFormat::PIXELFORMAT_RGBA8);
        auto output = colorTexture(device.get(), "oit-output", PixelFormat::PIXELFORMAT_RGBA8);
        RenderTargetOptions oitOptions;
        oitOptions.graphicsDevice = device.get();
        oitOptions.colorBuffers = {accumulation.get(), coverage.get()};
        oitOptions.depth = false;
        oitOptions.name = "oit-target";
        const auto oitTarget = device->createRenderTarget(oitOptions);
        RenderTargetOptions outputOptions;
        outputOptions.graphicsDevice = device.get();
        outputOptions.colorBuffer = output.get();
        outputOptions.depth = false;
        outputOptions.name = "oit-output-target";
        const auto outputTarget = device->createRenderTarget(outputOptions);

        const LayerParams red{{1.0f, 0.0f, 0.0f, 1.0f}, {0.5f, 2.0f, 0.0f, 0.0f}};
        const LayerParams blue{{0.0f, 0.0f, 1.0f, 1.0f}, {0.6f, 1.0f, 0.0f, 0.0f}};

        const auto composite = [&](const LayerParams& first, const LayerParams& second) {
            const Color zero(0.0f, 0.0f, 0.0f, 0.0f);
            const Color background(0.0f, 1.0f, 0.0f, 1.0f);
            device->setDepthState(DepthState::noDepth());
            device->setCullMode(CullMode::CULLFACE_NONE);
            device->frameStart();
            {
                RenderPass pass(shared);
                pass.init(oitTarget);
                pass.setClearColor(&zero);
                device->startRenderPass(&pass);
                device->setBlendState(RenderPassOit::accumulationBlendState());
                for (const LayerParams* params : {&first, &second}) {
                    QuadRender quad(layerShader);
                    quad.setUniforms(*params);
                    quad.render();
                }
                device->endRenderPass(&pass);
            }
            {
                RenderPass pass(shared);
                pass.init(outputTarget);
                pass.setClearColor(&background);
                device->startRenderPass(&pass);
                device->setBlendState(RenderPassOit::compositeBlendState());
                QuadRender quad(compositeShader);
                quad.setTexture(0, accumulation.get());
                quad.setTexture(1, coverage.get());
                quad.render();
                device->endRenderPass(&pass);
            }
            device->frameEnd();
            return std::pair{readPixel(output.get(), kSize / 4, kSize / 2), readPixel(output.get(), kSize * 3 / 4, kSize / 2)};
        };

        const auto [covered, uncovered] = composite(red, blue);
        check(near(covered, 0.5f, 0.2f, 0.3f),
            "two layers composite to the weighted average by the coverage, (127.5, 51, 76.5): " + describe(covered));
        check(near(uncovered, 0.0f, 1.0f, 0.0f), "a pixel no layer covers keeps the background: " + describe(uncovered));
        const auto [swapped, swappedUncovered] = composite(blue, red);
        check(std::abs(swapped.r - covered.r) <= 1 && std::abs(swapped.g - covered.g) <= 1 &&
                std::abs(swapped.b - covered.b) <= 1,
            "drawn the other way round, the same pixel: " + describe(swapped));
        (void)swappedUncovered;
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("oit gpu");
}
