// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// Compute::setupDispatchThreads on a real device of this build's backend. A Slang kernel
// handed over as a string adds index + 1 into slot `index` of a buffer for every thread the
// `vtdispatch` module says is in range, and the buffer is read back: each requested thread
// must have run exactly once (an atomic add, so a duplicate shows as a wrong value) and no
// slot past them must have been touched. The workgroup size is never given to the Compute:
// the dispatch must take it from the shader, which Metal knows from Slang's reflection and
// Vulkan from the SPIR-V. Run for a 1D count that is not a multiple of the workgroup, a 2D
// and a 3D grid, and a 1D dispatch spread over rows of workgroups as one that needs more of
// them than a dimension holds is (both backends allow 2^30 a dimension, so the spread is
// the one setupDispatchThreads makes under a small limit, dispatched as it is).
//
// Also the bundle's own kernels report the size their source declares, and each
// computeDispatch call is counted into the frame counters as one compute pass of its
// dispatches.
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
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/graphics/compute.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/shader-lib/slangShaders.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // One buffer at buffer(0) / binding 0 and the uniform block after it (compute.h's table).
    std::string kernelSource(const std::array<uint32_t, 3>& size)
    {
        return R"(
import vtdispatch;

struct Params
{
    uint4 dispatch;   // Compute::dispatchThreads()
};

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> outputs : register(u0);
[[vk::binding(1, 0)]] ConstantBuffer<Params> params : register(b1);

[shader("compute")]
[numthreads()" + std::to_string(size[0]) + ", " + std::to_string(size[1]) + ", " + std::to_string(size[2]) + R"()]
void writeIndices(uint3 id : SV_DispatchThreadID)
{
    if (!dispatchThreadInRange(id, params.dispatch)) {
        return;
    }
    const uint index = dispatchThreadIndex(id, params.dispatch);
    uint previous;
    InterlockedAdd(outputs[index], index + 1, previous);
}
)";
    }

    struct Params
    {
        std::array<uint32_t, 4> dispatch;
    };

    std::string text(const std::array<uint32_t, 3>& v)
    {
        return "[" + std::to_string(v[0]) + ", " + std::to_string(v[1]) + ", " + std::to_string(v[2]) + "]";
    }

    /// Dispatches `threads` through a kernel of workgroup `size` and checks the buffer.
    /// With `spread`, the dispatch is set up by hand as setupDispatchThreads spreads it under a
    /// small limit: `expectedGroups` workgroups and that uint4.
    void run(GraphicsDevice* device, const std::array<uint32_t, 3>& size, const std::array<uint32_t, 3>& threads,
        const std::array<uint32_t, 3>& expectedGroups, const std::optional<std::array<uint32_t, 4>>& spread = {})
    {
        const std::string label = text(threads) + " threads in workgroups of " + text(size);
        const std::string name = "dispatch-threads-" + std::to_string(size[0]) + "x" + std::to_string(size[1]) + "x" +
            std::to_string(size[2]);
        const auto shader = getOrCreateSlangShaderFromSource(device, name, kernelSource(size));
        check(shader != nullptr, label + ": the kernel compiles");
        if (!shader) {
            return;
        }
        check(shader->computeThreadGroupSize() == size,
            label + ": the shader knows its declared workgroup size, " + text(shader->computeThreadGroupSize()));

        const uint64_t total = static_cast<uint64_t>(threads[0]) * threads[1] * threads[2];
        const uint32_t slack = 256;   // slots past the threads, which nothing may touch
        const auto count = static_cast<int>(total + slack);
        const auto format = std::make_shared<VertexFormat>(4, true, false);
        VertexBufferOptions options;
        options.data.assign(static_cast<size_t>(count) * 4u, 0);
        const auto buffer = device->createVertexBuffer(format, count, options);

        Compute compute(device, shader, "DispatchThreads");
        compute.setParameter("outputs", buffer);
        if (spread) {
            compute.setupDispatch(expectedGroups[0], expectedGroups[1], expectedGroups[2]);
        } else {
            compute.setupDispatchThreads(threads[0], threads[1], threads[2]);
        }
        check(std::array<uint32_t, 3>{compute.dispatchX(), compute.dispatchY(), compute.dispatchZ()} == expectedGroups,
            label + ": " + text({compute.dispatchX(), compute.dispatchY(), compute.dispatchZ()}) + " workgroups, " +
                text(expectedGroups) + " expected");
        const Params params{spread ? *spread : compute.dispatchThreads()};
        compute.setUniformBlock(&params, sizeof(params));

        const FrameCounters::Compute before = device->frameCounters().compute;
        device->computeDispatch({&compute}, "DispatchThreads");
        const FrameCounters::Compute after = device->frameCounters().compute;
        check(after.dispatches == before.dispatches + 1 && after.passes == before.passes + 1,
            label + ": the call counts one dispatch in one compute pass");

        std::vector<uint32_t> values(static_cast<size_t>(count));
        const bool read = buffer && buffer->read(0, values.size() * 4u, values.data());
        size_t wrong = 0;
        size_t firstWrong = values.size();
        for (size_t i = 0; i < values.size(); ++i) {
            const uint32_t expected = i < total ? static_cast<uint32_t>(i + 1) : 0u;
            if (values[i] != expected) {
                if (wrong++ == 0) {
                    firstWrong = i;
                }
            }
        }
        check(read && wrong == 0, label + ": every thread ran exactly once and none past them" +
            (wrong ? " (" + std::to_string(wrong) + " slots wrong, the first at " + std::to_string(firstWrong) +
                ": " + std::to_string(values[firstWrong]) + ")" : std::string()));
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
    SDL_Window* window = SDL_CreateWindow("compute-dispatch-threads", 64, 64, flags);
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
        const uint32_t maxGroups = device->maxComputeWorkgroupsPerDimension();
        std::cout << "    at most " << maxGroups << " workgroups per dimension\n";

        std::cout << "\nthe bundle's kernels know their workgroup size\n";
        for (const auto& [program, size] : std::vector<std::pair<std::string, std::array<uint32_t, 3>>>{
                 {"instance-cull", {64, 1, 1}}, {"instance-cull-reset", {1, 1, 1}}, {"particle-sim", {256, 1, 1}},
                 {"particle-sort", {256, 1, 1}}}) {
            const auto shader = getOrCreateSlangShader(device.get(), program);
            check(shader && shader->computeThreadGroupSize() == size,
                program + ": " + (shader ? text(shader->computeThreadGroupSize()) : std::string("no shader")) +
                    ", as its [numthreads] says");
        }

        std::cout << "\nthreads, in whole workgroups\n";
        run(device.get(), {64, 1, 1}, {1000, 1, 1}, {16, 1, 1});
        run(device.get(), {64, 1, 1}, {64, 1, 1}, {1, 1, 1});
        run(device.get(), {8, 8, 1}, {37, 23, 1}, {5, 3, 1});
        run(device.get(), {4, 4, 4}, {9, 5, 3}, {3, 2, 1});

        std::cout << "\na one-dimensional dispatch past the per-dimension limit, spread over rows\n";
        // Both backends allow 2^30 workgroups a dimension, too many to reach with a test
        // buffer, so the spread setupDispatchThreads makes under a limit of 4 (641 threads of
        // 64 as 3 rows of 4 workgroups, rows of 256 threads: computeDispatchThreadsTests) is
        // dispatched as it is, and the kernel must index those rows.
        run(device.get(), {64, 1, 1}, {641, 1, 1}, {4, 3, 1}, std::array<uint32_t, 4>{641, 1, 1, 256});
        // And past the device's own limit where that is small enough to reach.
        if (maxGroups <= 65535u * 2u) {
            const uint32_t threads = maxGroups * 64u + 1000u;
            const uint32_t groups = (threads + 63u) / 64u;
            run(device.get(), {64, 1, 1}, {threads, 1, 1}, {(groups + 1u) / 2u, 2, 1});
        }

        std::cout << "\ncompute passes and dispatches\n";
        {
            const auto shader = getOrCreateSlangShaderFromSource(device.get(), "dispatch-threads-64x1x1",
                kernelSource({64, 1, 1}));
            const auto format = std::make_shared<VertexFormat>(4, true, false);
            VertexBufferOptions bufferOptions;
            bufferOptions.data.assign(64u * 4u, 0);
            const auto buffer = device->createVertexBuffer(format, 64, bufferOptions);
            Compute first(device.get(), shader, "First");
            Compute second(device.get(), shader, "Second");
            for (Compute* compute : {&first, &second}) {
                compute->setParameter("outputs", buffer);
                compute->setupDispatchThreads(8);
                const Params params{compute->dispatchThreads()};
                compute->setUniformBlock(&params, sizeof(params));
            }
            const FrameCounters::Compute before = device->frameCounters().compute;
            device->computeDispatch({&first, &second}, "Two");
            const FrameCounters::Compute after = device->frameCounters().compute;
            check(after.dispatches == before.dispatches + 2 && after.passes == before.passes + 1,
                "two computes in one call: two dispatches, one compute pass");
            std::vector<uint32_t> values(8);
            check(buffer->read(0, 32, values.data()) && values[7] == 16u, "and both ran (thread 7 added 8 twice)");
        }
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("compute-dispatch-threads-gpu");
}
