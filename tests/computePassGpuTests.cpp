// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// A ComputePass in the middle of a frame, on a real device of this build's backend: one
// pass clears a target red, a compute pass copies the target's texel into a buffer, and a
// third pass clears the same target blue, all in one frame. The kernel must see red: the
// pass before it has run and the pass after it has not. Run for two frames, so a dispatch
// that ran ahead of its frame would read the previous frame's blue (or, in the first,
// whatever the target held before anything rendered).
//
// Vulkan used to queue a dispatch into its upload queue, which is submitted on its own the
// moment it is flushed, ahead of the frame's command buffer that is submitted only at the
// frame's end: a dispatch mid-frame ran before every pass the frame had recorded.
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
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/graphics/compute.h"
#include "platform/graphics/computePass.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/frameGraph.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // One buffer and one sampled texture: Metal buffer(0) and texture(0), Vulkan bindings 0
    // and 1 (the table in compute.h).
    constexpr const char* kCopyTexelMetal = R"(
#include <metal_stdlib>
using namespace metal;

kernel void copyTexel(device float4* result [[buffer(0)]],
    texture2d<float, access::read> source [[texture(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    result[0] = source.read(uint2(1, 1));
}
)";

    constexpr const char* kCopyTexelGlsl = R"(
#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) buffer Result { vec4 result[]; };
layout(set = 0, binding = 1) uniform sampler2D source;
void main()
{
    result[0] = texelFetch(source, ivec2(1, 1), 0);
}
)";

    bool near(const std::array<float, 4>& value, const std::array<float, 4>& expected)
    {
        for (int i = 0; i < 4; ++i) {
            if (std::abs(value[i] - expected[i]) > 0.01f) {
                return false;
            }
        }
        return true;
    }

    std::string text(const std::array<float, 4>& value)
    {
        return "(" + std::to_string(value[0]) + ", " + std::to_string(value[1]) + ", " +
            std::to_string(value[2]) + ", " + std::to_string(value[3]) + ")";
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
    SDL_Window* window = SDL_CreateWindow("compute-pass", 64, 64, flags);
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
        check(device->supportsCompute(), "the device supports compute");

        TextureOptions textureOptions;
        textureOptions.name = "compute-pass-source";
        textureOptions.width = 4;
        textureOptions.height = 4;
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = false;
        auto texture = std::make_shared<Texture>(device.get(), textureOptions);
        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device.get();
        targetOptions.colorBuffer = texture.get();
        targetOptions.name = "compute-pass-target";
        const auto target = device->createRenderTarget(targetOptions);

        const auto format = std::make_shared<VertexFormat>(16, true, false);
        VertexBufferOptions bufferOptions;
        bufferOptions.data.assign(16, 0);
        const std::shared_ptr<VertexBuffer> result = device->createVertexBuffer(format, 1, bufferOptions);

        ShaderDefinition definition;
        definition.name = "compute-pass-copy-texel";
        definition.cshader = "copyTexel";
        const auto shader = createShader(device.get(), definition,
            device->shaderLanguage() == ShaderLanguage::Glsl ? kCopyTexelGlsl : kCopyTexelMetal);
        check(shader != nullptr && result != nullptr, "the kernel and its buffer are created");
        if (!shader || !result) {
            return 1;
        }
        Compute compute(device.get(), shader, "CopyTexel");
        compute.setParameter("result", result);
        compute.setParameter("source", texture.get());
        compute.setThreadgroupSize(1, 1, 1);
        compute.setupDispatch(1, 1, 1);

        // The passes borrow the device, which outlives them.
        const std::shared_ptr<GraphicsDevice> shared(device.get(), [](GraphicsDevice*) {});
        const auto clearPass = [&](const Color& colour) {
            auto pass = std::make_shared<RenderPass>(shared);
            pass->init(target);
            pass->setClearColor(&colour);
            pass->colorOps()->store = true;
            return pass;
        };
        const auto red = clearPass(Color(1.0f, 0.0f, 0.0f, 1.0f));
        const auto blue = clearPass(Color(0.0f, 0.0f, 1.0f, 1.0f));
        const auto computePass = std::make_shared<ComputePass>(shared, std::vector<Compute*>{&compute});
        computePass->setName("CopyTexel");

        for (int frame = 0; frame < 2; ++frame) {
            FrameGraph graph;
            graph.addRenderPass(red);
            graph.addRenderPass(computePass);
            graph.addRenderPass(blue);
            device->frameStart();
            graph.render(device.get());
            device->frameEnd();

            std::array<float, 4> seen{};
            const bool read = result->read(0, sizeof(seen), seen.data());
            check(read && near(seen, {1.0f, 0.0f, 0.0f, 1.0f}),
                "frame " + std::to_string(frame) + ": the kernel sees the pass before it (red), not the one after: "
                    + text(seen));

            std::vector<uint8_t> pixels;
            check(texture->read(pixels, 1, 1, 1, 1) && pixels.size() >= 4 && pixels[0] == 0 && pixels[2] == 255,
                "frame " + std::to_string(frame) + ": and the pass after it then turned the target blue");
        }
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("compute pass gpu");
}
