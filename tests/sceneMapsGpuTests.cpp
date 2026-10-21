// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// A compute kernel reading a camera's scene maps through the `vtscenemaps` module, on a real
// device of this build's backend. A frame clears a colour and a depth target, a ComputePass
// after it runs a kernel that writes what the module returns into a buffer, and the buffer
// is compared with the values the clears and the camera imply: the linear depth, the clip
// planes, the world position under a texel (against the camera's own inverse
// view-projection), a loaded and a filtered colour, and the gamma decode the handle asks for.
//
// It holds the fixed slots the module and Compute share (Metal texture 30 / 31, sampler 15,
// buffer 30; Vulkan set 1), which no other test reaches, and that the maps are read as they
// were rendered in the same frame. A Slang kernel, so it needs the source tree at run time.
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
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/camera.h"
#include "scene/graphNode.h"
#include "scene/frameGraph.h"
#include "scene/shader-lib/slangShaders.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // The buffer "result" is the kernel's only parameter: Metal buffer 0, Vulkan binding 0.
    constexpr const char* kReadSceneMaps = R"slang(
import vtscenemaps;

[[vk::binding(0, 0)]] RWStructuredBuffer<float4> result : register(u0);

[shader("compute")]
[numthreads(1, 1, 1)]
void readSceneMaps(uint3 id : SV_DispatchThreadID)
{
    const int2 texel = int2(1, 2);
    result[0] = float4(sceneDepthLinear(texel), sceneDepthNearClip(), sceneDepthFarClip(), float(sceneDepthSize().x));
    result[1] = float4(sceneDepthWorldPosition(texel), float(sceneDepthViewport().z));
    result[2] = sceneColorLoad(texel, 0);
    result[3] = sceneColorSample(float2(0.5, 0.5), 0.0);
    result[4] = float4(sceneColorToLinear(float3(0.5, 0.25, 1.0)), float(sceneColorSize(0).y));
}
)slang";

    bool close(const float value, const float expected, const float tolerance)
    {
        return std::abs(value - expected) <= tolerance;
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
    SDL_Window* window = SDL_CreateWindow("scene-maps", 64, 64, flags);
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
        if (!device || !device->supportsCompute()) {
            return 1;
        }

        constexpr uint32_t kWidth = 4;
        constexpr uint32_t kHeight = 4;
        TextureOptions colorOptions;
        colorOptions.name = "scene-maps-color";
        colorOptions.width = kWidth;
        colorOptions.height = kHeight;
        colorOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        colorOptions.mipmaps = false;
        auto color = std::make_shared<Texture>(device.get(), colorOptions);
        TextureOptions depthOptions = colorOptions;
        depthOptions.name = "scene-maps-depth";
        depthOptions.format = PixelFormat::PIXELFORMAT_DEPTH;
        depthOptions.minFilter = FilterMode::FILTER_NEAREST;
        depthOptions.magFilter = FilterMode::FILTER_NEAREST;
        auto depth = std::make_shared<Texture>(device.get(), depthOptions);
        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device.get();
        targetOptions.colorBuffer = color.get();
        targetOptions.depthBuffer = depth.get();
        targetOptions.name = "scene-maps-target";
        const auto target = device->createRenderTarget(targetOptions);

        const auto format = std::make_shared<VertexFormat>(16, true, false);
        VertexBufferOptions bufferOptions;
        bufferOptions.data.assign(16 * 5, 0);
        const std::shared_ptr<VertexBuffer> result = device->createVertexBuffer(format, 5, bufferOptions);

        const auto shader = getOrCreateSlangShaderFromSource(device.get(), "scene-maps-read", kReadSceneMaps);
        check(shader != nullptr && result != nullptr, "the kernel and its buffer are created");
        if (!shader || !result) {
            return 1;
        }

        // The camera the maps are published for: perspective, near 1, far 100, at the origin.
        Camera camera;
        camera.setNearClip(1.0f);
        camera.setFarClip(100.0f);
        camera.setAspectRatio(1.0f);
        const Matrix4 viewProjection = camera.projectionMatrix();
        camera.storeShaderMatrices(viewProjection, 0.0f, 0.0f, 1);

        Compute compute(device.get(), shader, "ReadSceneMaps");
        compute.setParameter("result", result);
        compute.setSceneDepthMap(camera.sceneDepthMapHandle());
        compute.setSceneColorMap(camera.sceneColorMapHandle());
        compute.setupDispatch(1, 1, 1);

        // The passes borrow the device, which outlives them.
        const std::shared_ptr<GraphicsDevice> shared(device.get(), [](GraphicsDevice*) {});
        auto clear = std::make_shared<RenderPass>(shared);
        clear->init(target);
        const Color clearColor(0.5f, 0.25f, 1.0f, 1.0f);
        constexpr float kClearDepth = 0.5f;
        clear->setClearColor(&clearColor);
        clear->setClearDepth(&kClearDepth);
        clear->colorOps()->store = true;
        clear->depthStencilOps()->storeDepth = true;
        const auto computePass = std::make_shared<ComputePass>(shared, std::vector<Compute*>{&compute});

        // the depth buffer's 0.5 at near 1 and far 100 is the linear depth n f / (f + d (n - f))
        const float linearDepth = 100.0f / (100.0f + 0.5f * (1.0f - 100.0f));
        // texel (1, 2) of 4 x 4: NDC (-0.25, -0.25), the depth remapped to GL clip z 0
        const Vector4 clip = viewProjection.inverse() * Vector4(-0.25f, -0.25f, 0.0f, 1.0f);
        const Vector3 world(clip.getX() / clip.getW(), clip.getY() / clip.getW(), clip.getZ() / clip.getW());

        for (int frame = 0; frame < 2; ++frame) {
            camera.publishSceneDepthMap(depth, frame, false, false, false);
            // gamma-encoded in the first frame, linear in the second
            camera.publishSceneColorMap(color, frame == 0);

            FrameGraph graph;
            graph.addRenderPass(clear);
            graph.addRenderPass(computePass);
            device->frameStart();
            graph.render(device.get());
            device->frameEnd();

            std::array<std::array<float, 4>, 5> seen{};
            check(result->read(0, sizeof(seen), seen.data()), "the result is read back");
            const std::string at = "frame " + std::to_string(frame) + ": ";
            check(close(seen[0][0], linearDepth, 1e-3f) && seen[0][1] == 1.0f && seen[0][2] == 100.0f &&
                seen[0][3] == static_cast<float>(kWidth),
                at + "the linear depth, the clip planes and the size " + text(seen[0]));
            check(close(seen[1][0], world.getX(), 1e-3f) && close(seen[1][1], world.getY(), 1e-3f) &&
                close(seen[1][2], world.getZ(), 1e-3f) && close(-seen[1][2], linearDepth, 1e-3f) &&
                seen[1][3] == static_cast<float>(kWidth),
                at + "the world position under the texel, at the linear depth " + text(seen[1]));
            check(close(seen[2][0], 0.5f, 0.01f) && close(seen[2][1], 0.25f, 0.01f) && close(seen[2][2], 1.0f, 0.01f),
                at + "the colour loaded as it was rendered " + text(seen[2]));
            check(close(seen[3][0], 0.5f, 0.01f) && close(seen[3][2], 1.0f, 0.01f),
                at + "and sampled " + text(seen[3]));
            const float decoded = frame == 0 ? std::pow(0.5f, 2.2f) : 0.5f;
            check(close(seen[4][0], decoded, 1e-4f) && seen[4][3] == static_cast<float>(kHeight),
                at + "the gamma decode follows the handle " + text(seen[4]));
        }
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("scene maps gpu");
}
