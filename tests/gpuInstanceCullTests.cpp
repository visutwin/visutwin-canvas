// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// GPU instance culling on a real device, whichever backend this build has: a row of
// instances is culled against two planes, and the indirect draw arguments and the
// compacted instance buffer are read back and compared with the same cull done on the
// CPU. No example renders with GPU culling on, so this is the only thing that runs it.
//
// Then through the renderer: two cameras that see different stretches of one instanced
// mesh each get an output of their own, culled to their own frustum, and a camera that
// drew nothing this frame gets none (its draw takes the whole buffer). With one output
// per mesh shared by every camera, each view would draw the other's set.
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
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/instanceCuller.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/renderer/forwardRenderer.h"
#include "support/check.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    struct Instance
    {
        float matrix[16];
        float color[4];
    };
    static_assert(sizeof(Instance) == 80);

    struct DrawArgs
    {
        uint32_t indexCount;
        uint32_t instanceCount;
        uint32_t indexStart;
        int32_t baseVertex;
        uint32_t baseInstance;
    };
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
    SDL_Window* window = SDL_CreateWindow("gpu-instance-cull", 64, 64, flags);
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
        check(device->supportsGpuInstanceCulling(), "the device supports GPU instance culling");

        // 64 instances along x at x = i, coloured by index so each can be identified.
        constexpr uint32_t kCount = 64;
        std::vector<Instance> instances(kCount);
        for (uint32_t i = 0; i < kCount; ++i) {
            Instance& instance = instances[i];
            std::memset(&instance, 0, sizeof(instance));
            instance.matrix[0] = instance.matrix[5] = instance.matrix[10] = instance.matrix[15] = 1.0f;
            instance.matrix[12] = static_cast<float>(i);
            instance.color[0] = static_cast<float>(i);
            instance.color[3] = 1.0f;
        }
        VertexBufferOptions bufferOptions;
        bufferOptions.data.resize(kCount * sizeof(Instance));
        std::memcpy(bufferOptions.data.data(), instances.data(), bufferOptions.data.size());
        auto format = std::make_shared<VertexFormat>(
            VertexFormat::INSTANCING_MATRIX_COLOR_SIZE, VertexFormat::instanceMatrixElements(), true, true);
        std::shared_ptr<VertexBuffer> input = device->createVertexBuffer(format, kCount, bufferOptions);

        // Keep 10 <= x <= 20, widened by the sphere radius 0.5: instances 10 .. 20.
        InstanceCullParams params{};
        for (auto& plane : params.frustumPlanes) {
            plane[3] = 1.0e6f;   // the other four planes take everything
        }
        params.frustumPlanes[0][0] = 1.0f;   // x - 10 >= -r
        params.frustumPlanes[0][3] = -10.0f;
        params.frustumPlanes[1][0] = -1.0f;  // 20 - x >= -r
        params.frustumPlanes[1][3] = 20.0f;
        params.boundingSphereRadius = 0.5f;
        params.instanceCount = kCount;
        params.indexCount = 36;
        params.indexStart = 6;
        params.baseVertex = 3;

        std::vector<uint32_t> expected;
        for (uint32_t i = 0; i < kCount; ++i) {
            const float x = static_cast<float>(i);
            if (x - 10.0f >= -params.boundingSphereRadius && 20.0f - x >= -params.boundingSphereRadius) {
                expected.push_back(i);
            }
        }

        std::unique_ptr<InstanceCuller> culler = device->createInstanceCuller();
        check(culler != nullptr, "a culler is created");
        if (!culler) {
            return 1;
        }
        culler->reserve(kCount);
        check(culler->compactedNativeBuffer() && culler->indirectArgsNativeBuffer(),
            "reserve() allocates the compacted and indirect-argument buffers");

        // Twice: the second run must start its count over, not add to the first.
        for (int run = 0; run < 2; ++run) {
            culler->cull(input, params);

            const auto argsFormat = std::make_shared<VertexFormat>(static_cast<int>(sizeof(DrawArgs)), true, false);
            auto args = device->createVertexBufferFromNativeBuffer(argsFormat, 1, culler->indirectArgsNativeBuffer());
            auto compacted = device->createVertexBufferFromNativeBuffer(format, static_cast<int>(kCount),
                culler->compactedNativeBuffer());
            DrawArgs drawArgs{};
            const bool readArgs = args && args->read(0, sizeof(DrawArgs), &drawArgs);
            check(readArgs, "run " + std::to_string(run) + ": the indirect arguments read back");
            check(drawArgs.instanceCount == expected.size(),
                "run " + std::to_string(run) + ": " + std::to_string(drawArgs.instanceCount) +
                " instances survive, " + std::to_string(expected.size()) + " expected");
            check(drawArgs.indexCount == params.indexCount && drawArgs.indexStart == params.indexStart &&
                    drawArgs.baseVertex == params.baseVertex && drawArgs.baseInstance == 0,
                "run " + std::to_string(run) + ": the draw arguments carry the mesh's index range");

            std::vector<Instance> out(std::min<uint32_t>(drawArgs.instanceCount, kCount));
            const bool readInstances = compacted && !out.empty() &&
                compacted->read(0, out.size() * sizeof(Instance), out.data());
            std::vector<uint32_t> survivors;
            bool intact = readInstances;
            for (const Instance& instance : out) {
                const auto index = static_cast<uint32_t>(instance.matrix[12]);
                survivors.push_back(index);
                intact = intact && std::memcmp(&instance, &instances[index], sizeof(Instance)) == 0;
            }
            std::sort(survivors.begin(), survivors.end());
            check(survivors == expected, "run " + std::to_string(run) + ": exactly the instances in range survive");
            check(intact, "run " + std::to_string(run) + ": each survivor is copied whole (matrix and colour)");
        }

        std::cout << "\nper camera, through the renderer\n";
        {
            auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(device);

            // The same 64 instances, now centred on x = 0 and 10 units in front of two
            // cameras at x = -20 and x = +20, each looking down -z with a narrow view.
            std::vector<Instance> row = instances;
            for (uint32_t i = 0; i < kCount; ++i) {
                row[i].matrix[12] = static_cast<float>(i) - 32.0f;
                row[i].matrix[14] = -10.0f;
            }
            VertexBufferOptions rowOptions;
            rowOptions.data.resize(kCount * sizeof(Instance));
            std::memcpy(rowOptions.data.data(), row.data(), rowOptions.data.size());
            auto rowBuffer = device->createVertexBuffer(format, kCount, rowOptions);

            auto mesh = std::make_shared<Mesh>();
            Primitive primitive;
            primitive.count = 36;
            mesh->setPrimitive(primitive);
            auto* holder = addTo(engine->root(), newEntity(engine.get(), "instances"));
            auto* render = static_cast<RenderComponent*>(holder->addComponent<RenderComponent>());
            auto* meshInstance = render->addMeshInstance(std::make_unique<MeshInstance>(mesh.get(), nullptr, holder));
            meshInstance->setInstancing(rowBuffer, static_cast<int>(kCount));
            meshInstance->enableGpuInstanceCulling(device.get(), 0.5f);
            check(meshInstance->gpuCullingEnabled(), "culling is enabled on the mesh instance");

            const auto makeCamera = [&](const char* name, const float x) {
                auto* entity = addTo(engine->root(), newEntity(engine.get(), name));
                entity->setPosition(x, 0.0f, 0.0f);
                auto* component = static_cast<CameraComponent*>(entity->addComponent<CameraComponent>());
                Camera* camera = component->camera();
                camera->setFov(40.0f);
                camera->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
                camera->setAspectRatio(1.0f);
                return camera;
            };
            Camera* left = makeCamera("left", -20.0f);
            Camera* right = makeCamera("right", 20.0f);
            Camera* idle = makeCamera("idle", 0.0f);

            // What each frustum keeps, by the same sphere-against-planes rule the kernel uses.
            const auto expectedFor = [&](Camera* camera) {
                const Matrix4 vp = camera->projectionMatrix() * camera->node()->worldTransform().inverse();
                float planes[6][4];
                InstanceCuller::extractFrustumPlanes(reinterpret_cast<const float*>(&vp), planes);
                std::vector<uint32_t> kept;
                for (uint32_t i = 0; i < kCount; ++i) {
                    bool inside = true;
                    for (const auto& plane : planes) {
                        inside = inside && plane[0] * row[i].matrix[12] + plane[1] * row[i].matrix[13] +
                            plane[2] * row[i].matrix[14] + plane[3] >= -0.5f;
                    }
                    if (inside) {
                        kept.push_back(i);
                    }
                }
                return kept;
            };
            const auto survivorsOf = [&](const MeshInstance::GpuCullOutput* output) {
                std::vector<uint32_t> result;
                if (!output) {
                    return result;
                }
                const auto argsFormat = std::make_shared<VertexFormat>(static_cast<int>(sizeof(DrawArgs)), true, false);
                auto args = device->createVertexBufferFromNativeBuffer(argsFormat, 1,
                    output->culler->indirectArgsNativeBuffer());
                DrawArgs drawArgs{};
                if (!args || !args->read(0, sizeof(DrawArgs), &drawArgs)) {
                    return result;
                }
                std::vector<Instance> out(std::min<uint32_t>(drawArgs.instanceCount, kCount));
                if (!out.empty() && output->compacted->read(0, out.size() * sizeof(Instance), out.data())) {
                    for (const Instance& instance : out) {
                        result.push_back(static_cast<uint32_t>(instance.matrix[12] + 32.0f));
                    }
                }
                std::sort(result.begin(), result.end());
                return result;
            };

            engine->renderer()->dispatchGpuInstanceCulling({left, right});
            const int version = device->renderVersion();
            const auto* leftOutput = meshInstance->culledOutput(left, version);
            const auto* rightOutput = meshInstance->culledOutput(right, version);
            check(leftOutput && rightOutput && leftOutput != rightOutput, "each camera has an output of its own");
            check(meshInstance->culledOutput(idle, version) == nullptr,
                "a camera that was not culled for has none, so its draw takes every instance");

            const auto leftExpected = expectedFor(left);
            const auto rightExpected = expectedFor(right);
            check(!leftExpected.empty() && !rightExpected.empty() && leftExpected != rightExpected,
                "the two cameras see different, non-empty stretches of the row");
            check(survivorsOf(leftOutput) == leftExpected, "the left camera's output keeps what it sees");
            check(survivorsOf(rightOutput) == rightExpected,
                "and the right camera's what it sees, not the left camera's");
        }
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("gpu instance cull");
}
