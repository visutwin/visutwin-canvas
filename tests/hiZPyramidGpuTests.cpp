// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
// The hierarchical depth (Hi-Z) pyramid on a real device of this build's backend
// (HiZPyramidGenerator, RenderPassHiZ, the Slang program `hiz-pyramid`). Every level of a
// pyramid is read back (Texture::read) and every texel is held, exactly, against a CPU
// reduction of the level above by the same coverage rule: the 2x2 block at twice the texel,
// and the last column or row of an odd-sized source with the last texel.
//
// Three sources: R32F textures of random depths at sizes that are odd, even, one texel wide
// and one tall (the reduction rule itself, every value distinct); a depth texture cleared to
// 0.25 with a rect cleared to 1 inside the pass (the depth read, which a depth texture allows
// only point-sampled, and its row order: the rect is not symmetric); and that depth again
// through a camera's handles — RenderPassHiZ in a frame graph publishing the pyramid, then a
// compute kernel reading it through the `vtscenemaps` module at its fixed slots (Metal
// texture 32, Vulkan set 1 binding 4), which no other test reaches. A Slang kernel, so it
// needs the source tree at run time.
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
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/math/vector4.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/computePass.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/sceneMapHandles.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/camera.h"
#include "scene/frameGraph.h"
#include "scene/graphNode.h"
#include "scene/graphics/hiZPyramid.h"
#include "scene/renderer/localShadowFace.h"
#include "scene/shader-lib/slangShaders.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // One level of the pyramid, CPU side: per texel the nearest and farthest depth.
    struct Level
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<std::array<float, 2>> texels;

        const std::array<float, 2>& at(const uint32_t x, const uint32_t y) const { return texels[y * width + x]; }
    };

    // The reduction the program does: the 2x2 block at twice the texel, widened to the source's
    // last column or row for the last texel.
    Level reduce(const Level& source)
    {
        Level level;
        level.width = std::max(source.width >> 1, 1u);
        level.height = std::max(source.height >> 1, 1u);
        level.texels.resize(static_cast<size_t>(level.width) * level.height);
        for (uint32_t y = 0; y < level.height; ++y) {
            for (uint32_t x = 0; x < level.width; ++x) {
                const uint32_t lastX = x == level.width - 1 ? source.width - 1 : std::min(2 * x + 1, source.width - 1);
                const uint32_t lastY = y == level.height - 1 ? source.height - 1 : std::min(2 * y + 1, source.height - 1);
                std::array<float, 2> range = {1.0f, 0.0f};
                for (uint32_t sy = 2 * y; sy <= lastY; ++sy) {
                    for (uint32_t sx = 2 * x; sx <= lastX; ++sx) {
                        range[0] = std::min(range[0], source.at(sx, sy)[0]);
                        range[1] = std::max(range[1], source.at(sx, sy)[1]);
                    }
                }
                level.texels[static_cast<size_t>(y) * level.width + x] = range;
            }
        }
        return level;
    }

    // The CPU pyramid of a depth map (one value per texel).
    std::vector<Level> referencePyramid(const std::vector<float>& depth, const uint32_t width, const uint32_t height,
        const uint32_t levels)
    {
        Level source{width, height, {}};
        for (const float d : depth) {
            source.texels.push_back({d, d});
        }
        std::vector<Level> pyramid;
        for (uint32_t level = 0; level < levels; ++level) {
            pyramid.push_back(reduce(level == 0 ? source : pyramid.back()));
        }
        return pyramid;
    }

    // Reads every level of `pyramid` back and compares it with `reference`, texel by texel.
    // Returns the levels read, for the compute comparison.
    std::vector<Level> checkPyramid(Texture* pyramid, const std::vector<Level>& reference, const std::string& what)
    {
        std::vector<Level> seen;
        check(pyramid && pyramid->getNumLevels() == reference.size(), what + ": " +
            std::to_string(reference.size()) + " levels, level 0 " + std::to_string(reference[0].width) + "x" +
            std::to_string(reference[0].height));
        if (!pyramid || pyramid->getNumLevels() != reference.size()) {
            return seen;
        }
        for (uint32_t level = 0; level < reference.size(); ++level) {
            const Level& expected = reference[level];
            std::vector<uint8_t> bytes;
            const bool read = pyramid->read(bytes, 0, 0, 0, 0, level);
            Level got{expected.width, expected.height, {}};
            got.texels.resize(expected.texels.size());
            if (read && bytes.size() == got.texels.size() * sizeof(float) * 2) {
                std::memcpy(got.texels.data(), bytes.data(), bytes.size());
            }
            size_t wrong = 0;
            std::string first;
            for (uint32_t y = 0; y < expected.height; ++y) {
                for (uint32_t x = 0; x < expected.width; ++x) {
                    if (got.at(x, y) != expected.at(x, y)) {
                        if (wrong++ == 0) {
                            first = " (first at " + std::to_string(x) + "," + std::to_string(y) + ": " +
                                std::to_string(got.at(x, y)[0]) + ".." + std::to_string(got.at(x, y)[1]) +
                                ", expected " + std::to_string(expected.at(x, y)[0]) + ".." +
                                std::to_string(expected.at(x, y)[1]) + ")";
                        }
                    }
                }
            }
            check(read && wrong == 0, what + ": level " + std::to_string(level) + " (" +
                std::to_string(expected.width) + "x" + std::to_string(expected.height) + ") exact, " +
                std::to_string(wrong) + " texels off" + first);
            seen.push_back(std::move(got));
        }
        return seen;
    }

    /// Clears its depth-only target to the clear depth, then a rect of it to 1 inside the pass.
    class DepthPatternPass final : public RenderPass
    {
    public:
        DepthPatternPass(const std::shared_ptr<GraphicsDevice>& device, const Vector4& rect)
            : RenderPass(device), _rect(rect)
        {
        }

        void execute() override { clearDepthRect(device().get(), _rect); }

    private:
        Vector4 _rect;
    };

    // The kernel writes what the module returns for the attached pyramid.
    constexpr const char* kReadHiZ = R"slang(
import vtscenemaps;

[[vk::binding(0, 0)]] RWStructuredBuffer<float4> result : register(u0);

[shader("compute")]
[numthreads(1, 1, 1)]
void readHiZ(uint3 id : SV_DispatchThreadID)
{
    const uint levels = sceneHiZLevels();
    const int top = max(int(levels) - 1, 0);
    result[0] = float4(float(levels), float(sceneHiZSize(0).x), float(sceneHiZSize(0).y),
        float(sceneHiZSize(top).x));
    result[1] = float4(float2(sceneHiZDepthSize()), float2(sceneHiZViewport().zw));
    result[2] = float4(sceneHiZLoad(int2(3, 2), 0), sceneHiZLoad(int2(2, 1), 1));
    result[3] = float4(sceneHiZLoad(int2(0, 0), top), sceneHiZLoad(int2(1, 0), 2));
    const float4x4 m = sceneHiZViewProjection();
    result[4] = mul(float4(1.0, 2.0, -3.0, 1.0), m);
}
)slang";
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
    SDL_Window* window = SDL_CreateWindow("hiz-pyramid", 64, 64, flags);
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
        // The passes borrow the device, which outlives them.
        const std::shared_ptr<GraphicsDevice> shared(device.get(), [](GraphicsDevice*) {});

        std::cout << "\nR32F depth maps of random values\n";
        std::mt19937 random(20261107u);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        HiZPyramidGenerator generator(shared);
        const std::array<std::array<uint32_t, 2>, 5> sizes = {{{37, 23}, {16, 16}, {1, 5}, {6, 1}, {3, 3}}};
        for (const auto& [width, height] : sizes) {
            std::vector<float> depth(static_cast<size_t>(width) * height);
            for (float& d : depth) {
                d = unit(random);
            }
            TextureOptions sourceOptions;
            sourceOptions.name = "hiz-source";
            sourceOptions.width = width;
            sourceOptions.height = height;
            sourceOptions.format = PixelFormat::PIXELFORMAT_R32F;
            sourceOptions.mipmaps = false;
            sourceOptions.minFilter = FilterMode::FILTER_NEAREST;
            sourceOptions.magFilter = FilterMode::FILTER_NEAREST;
            auto source = std::make_shared<Texture>(device.get(), sourceOptions);
            source->setLevelData(0, reinterpret_cast<const uint8_t*>(depth.data()), depth.size() * sizeof(float));
            source->upload();

            device->frameStart();
            const auto pyramid = generator.generate(source.get());
            device->frameEnd();

            const uint32_t levels = pyramid ? pyramid->getNumLevels() : 1u;
            checkPyramid(pyramid.get(), referencePyramid(depth, width, height, levels),
                std::to_string(width) + "x" + std::to_string(height));
        }

        std::cout << "\na depth texture, through a camera's handles and a compute kernel\n";
        constexpr uint32_t kWidth = 29;
        constexpr uint32_t kHeight = 19;
        constexpr float kClearDepth = 0.25f;
        // x 5..17, y 3..11 (rows from the top): not symmetric, so a flipped read shows
        const Vector4 rect(5.0f, 3.0f, 13.0f, 9.0f);
        TextureOptions depthOptions;
        depthOptions.name = "hiz-depth";
        depthOptions.width = kWidth;
        depthOptions.height = kHeight;
        depthOptions.format = PixelFormat::PIXELFORMAT_DEPTH;
        depthOptions.mipmaps = false;
        depthOptions.minFilter = FilterMode::FILTER_NEAREST;
        depthOptions.magFilter = FilterMode::FILTER_NEAREST;
        auto depthTexture = std::make_shared<Texture>(device.get(), depthOptions);
        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device.get();
        targetOptions.depthBuffer = depthTexture.get();
        targetOptions.name = "hiz-depth-target";
        const auto target = device->createRenderTarget(targetOptions);
        prepareClearDepthShader(device.get());

        auto pattern = std::make_shared<DepthPatternPass>(shared, rect);
        pattern->init(target);
        pattern->setClearDepth(&kClearDepth);
        pattern->depthStencilOps()->storeDepth = true;

        Camera camera;
        camera.setNearClip(1.0f);
        camera.setFarClip(50.0f);
        camera.setAspectRatio(1.0f);
        const Matrix4 viewProjection = camera.projectionMatrix();
        camera.storeShaderMatrices(viewProjection, 0.0f, 0.0f, 1);
        auto hiZPass = std::make_shared<RenderPassHiZ>(shared, camera.sceneDepthMapHandle(), camera.hiZMapHandle());

        const auto format = std::make_shared<VertexFormat>(16, true, false);
        VertexBufferOptions bufferOptions;
        bufferOptions.data.assign(16 * 5, 0);
        const std::shared_ptr<VertexBuffer> result = device->createVertexBuffer(format, 5, bufferOptions);
        const auto shader = getOrCreateSlangShaderFromSource(device.get(), "hiz-read", kReadHiZ);
        check(shader != nullptr && result != nullptr, "the kernel and its buffer are created");
        if (!shader || !result) {
            return 1;
        }
        Compute compute(device.get(), shader, "ReadHiZ");
        compute.setParameter("result", result);
        compute.setHiZMap(camera.hiZMapHandle());
        compute.setupDispatch(1, 1, 1);
        const auto computePass = std::make_shared<ComputePass>(shared, std::vector<Compute*>{&compute});

        device->frameStart();
        camera.publishSceneDepthMap(depthTexture, device->renderVersion(), false, false, false);
        {
            FrameGraph graph;
            graph.addRenderPass(pattern);
            graph.addRenderPass(hiZPass);
            graph.addRenderPass(computePass);
            graph.render(device.get());
        }
        device->frameEnd();

        std::vector<float> depth(static_cast<size_t>(kWidth) * kHeight, kClearDepth);
        for (uint32_t y = 3; y < 12; ++y) {
            for (uint32_t x = 5; x < 18; ++x) {
                depth[static_cast<size_t>(y) * kWidth + x] = 1.0f;
            }
        }
        const auto handle = camera.hiZMapHandle();
        const auto pyramid = handle->texture.lock();
        check(pyramid != nullptr && handle->renderVersion == device->renderVersion() &&
            handle->depthWidth == kWidth && handle->depthHeight == kHeight && handle->levels == 4,
            "the pass published the pyramid of this frame's depth: 29x19 depth, 4 levels from 14x9");
        const auto reference = referencePyramid(depth, kWidth, kHeight, pyramid ? pyramid->getNumLevels() : 1u);
        const auto seen = checkPyramid(pyramid.get(), reference, "29x19 depth");

        std::array<std::array<float, 4>, 5> read{};
        check(result->read(0, sizeof(read), read.data()), "the kernel's result is read back");
        check(read[0][0] == 4.0f && read[0][1] == 14.0f && read[0][2] == 9.0f && read[0][3] == 1.0f,
            "the kernel sees 4 levels, level 0 of 14x9, the last 1 wide");
        check(read[1][0] == 29.0f && read[1][1] == 19.0f && read[1][2] == 29.0f && read[1][3] == 19.0f,
            "the depth's size and viewport");
        if (seen.size() == 4) {
            check(read[2][0] == seen[0].at(3, 2)[0] && read[2][1] == seen[0].at(3, 2)[1] &&
                read[2][2] == seen[1].at(2, 1)[0] && read[2][3] == seen[1].at(2, 1)[1] &&
                read[3][0] == seen[3].at(0, 0)[0] && read[3][1] == seen[3].at(0, 0)[1] &&
                read[3][2] == seen[2].at(1, 0)[0] && read[3][3] == seen[2].at(1, 0)[1],
                "sceneHiZLoad reads the levels as the readback does");
        }
        const Vector4 projected = viewProjection * Vector4(1.0f, 2.0f, -3.0f, 1.0f);
        const auto near = [](const float a, const float b) { return std::abs(a - b) <= 1e-4f * std::max(1.0f, std::abs(b)); };
        check(near(read[4][0], projected.getX()) && near(read[4][1], projected.getY()) &&
            near(read[4][2], projected.getZ()) && near(read[4][3], projected.getW()),
            "sceneHiZViewProjection projects a point as the camera's matrix does");

        std::cout << "\na compute with the pyramid attached before any is built\n";
        Camera unbuilt;
        Compute fresh(device.get(), shader, "ReadHiZFresh");
        fresh.setParameter("result", result);
        fresh.setHiZMap(unbuilt.hiZMapHandle());
        fresh.setupDispatch(1, 1, 1);
        const auto freshPass = std::make_shared<ComputePass>(shared, std::vector<Compute*>{&fresh});
        device->frameStart();
        {
            FrameGraph graph;
            graph.addRenderPass(freshPass);
            graph.render(device.get());
        }
        device->frameEnd();
        check(result->read(0, sizeof(read), read.data()), "the kernel's result is read back");
        check(read[0][0] == 0.0f && read[3][0] == 0.0f && read[3][1] == 1.0f,
            "no pyramid: 0 levels, and the substitute's range 0..1 occludes nothing");
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("hiz pyramid gpu");
}
