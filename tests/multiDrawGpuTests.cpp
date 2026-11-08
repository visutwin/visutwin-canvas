// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
// GraphicsDevice::multiDraw on a real device of this build's backend: one call over a sub-draw
// list, which Vulkan issues as one indirect draw call (supportsMultiDrawIndirect) and Metal as a
// draw call per sub-draw with the state bound once. A 64x16 target is eight cells of 8x16; a
// quad covers one cell and the vertex stage moves each instance a cell to the right per instance
// index, which counts from the sub-draw's first instance. The list's sub-draws differ in every
// field of the record: the first index (the first six indices are a degenerate quad, so a
// sub-draw that ignored its first index would draw nothing), the base vertex (the second quad of
// the buffer starts one cell further), the instance count and the first instance; the cells they
// cover are 0, 3, 4 and 6, and the others must stay clear. Indexed and non-indexed lists both,
// and a list of one.
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
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/math/color.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/multiDrawCommand.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/shader-lib/slangShaders.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 16;
    constexpr uint32_t kCells = 8;

    // Each instance a cell (a quarter of NDC's width over eight cells) to the right.
    constexpr const char* kProgram = R"(
import vtquad;
#include "bindings.slang"

struct VSOut { float4 clip : SV_Position; };

[shader("vertex")]
VSOut cellVertex(QuadVertexIn input, uint instance : SV_VulkanInstanceID)
{
    VSOut output;
    output.clip = float4(input.position.x + float(instance) * 0.25, input.position.y, 0.5, 1.0);
    return output;
}

[shader("fragment")]
float4 cellFragment(VSOut input) : SV_Target0
{
    return float4(1.0, 1.0, 1.0, 1.0);
}
)";

    struct Vertex
    {
        float position[3];
        float normal[3];
        float uv0[2];
        float tangent[4];
        float uv1[2];
    };
    static_assert(sizeof(Vertex) == 56);

    // The corners of cell `cell` in NDC (x from -1 by 0.25 a cell, the full height).
    std::array<Vertex, 4> cellCorners(const int cell)
    {
        const float x0 = -1.0f + 0.25f * static_cast<float>(cell);
        const float x1 = x0 + 0.25f;
        std::array<Vertex, 4> corners{};
        const float xs[4] = {x0, x1, x1, x0};
        const float ys[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        for (int i = 0; i < 4; ++i) {
            corners[i].position[0] = xs[i];
            corners[i].position[1] = ys[i];
            corners[i].normal[2] = 1.0f;
            corners[i].tangent[0] = corners[i].tangent[3] = 1.0f;
        }
        return corners;
    }

    std::shared_ptr<VertexBuffer> vertexBuffer(GraphicsDevice* device, const std::vector<Vertex>& vertices)
    {
        auto format = std::make_shared<VertexFormat>(56, VertexFormat::standardElements(), true, false);
        VertexBufferOptions options;
        options.usage = BUFFER_STATIC;
        options.data.resize(vertices.size() * sizeof(Vertex));
        std::memcpy(options.data.data(), vertices.data(), options.data.size());
        return device->createVertexBuffer(format, static_cast<int>(vertices.size()), options);
    }

    // Which of the eight cells hold the quad's white, read at each cell's centre.
    std::string coveredCells(Texture* color)
    {
        std::vector<uint8_t> pixels;
        if (!color->read(pixels) || pixels.size() != static_cast<size_t>(kWidth) * kHeight * 4) {
            return "unreadable";
        }
        std::string cells;
        for (uint32_t cell = 0; cell < kCells; ++cell) {
            const size_t i = (static_cast<size_t>(kHeight / 2) * kWidth + cell * (kWidth / kCells) + kWidth / kCells / 2) * 4;
            cells += pixels[i] > 128 ? '#' : '.';
        }
        return cells;
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
    SDL_Window* window = SDL_CreateWindow("multi-draw", 64, 64, flags);
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
        std::cout << "    one indirect draw call per list: " << (device->supportsMultiDrawIndirect() ? "yes" : "no") << "\n";
        const std::shared_ptr<GraphicsDevice> shared(device.get(), [](GraphicsDevice*) {});
        const auto shader = getOrCreateSlangShaderFromSource(device.get(), "multi-draw-cells", kProgram);
        check(shader != nullptr, "the cell program compiles");
        if (!shader) {
            return 1;
        }

        TextureOptions colorOptions;
        colorOptions.name = "multi-draw-color";
        colorOptions.width = kWidth;
        colorOptions.height = kHeight;
        colorOptions.mipmaps = false;
        auto color = std::make_unique<Texture>(device.get(), colorOptions);
        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device.get();
        targetOptions.colorBuffer = color.get();
        targetOptions.depth = false;
        targetOptions.name = "multi-draw-target";
        const auto target = device->createRenderTarget(targetOptions);
        const Color clearColor(0.0f, 0.0f, 0.0f, 1.0f);

        const auto drawList = [&](const std::shared_ptr<VertexBuffer>& vertices,
                                  const std::shared_ptr<IndexBuffer>& indices,
                                  const std::vector<MultiDrawCommand>& commands) {
            RenderPass pass(shared);
            pass.init(target);
            pass.setClearColor(&clearColor);
            device->setBlendState(BlendState::noBlend());
            device->setDepthState(DepthState::noDepth());
            device->setCullMode(CullMode::CULLFACE_NONE);
            device->frameStart();
            device->startRenderPass(&pass);
            device->setMaterial(nullptr);
            device->setVertexBuffer(vertices);
            device->setShader(shader);
            Primitive primitive;
            primitive.type = PRIMITIVE_TRIANGLES;
            primitive.indexed = indices != nullptr;
            device->multiDraw(primitive, indices, commands);
            device->endRenderPass(&pass);
            device->frameEnd();
            return coveredCells(color.get());
        };

        // The quads of cells 0 and 1, four corners each.
        std::vector<Vertex> corners;
        for (const int cell : {0, 1}) {
            const auto quad = cellCorners(cell);
            corners.insert(corners.end(), quad.begin(), quad.end());
        }
        const auto indexedVertices = vertexBuffer(device.get(), corners);
        // Six degenerate indices, then the quad's two triangles.
        const std::vector<uint16_t> indexData = {0, 0, 0, 0, 0, 0, 0, 1, 2, 0, 2, 3};
        std::vector<uint8_t> indexBytes(indexData.size() * sizeof(uint16_t));
        std::memcpy(indexBytes.data(), indexData.data(), indexBytes.size());
        const auto indices = device->createIndexBuffer(INDEXFORMAT_UINT16, static_cast<int>(indexData.size()), indexBytes);

        std::cout << "\nan indexed list\n";
        // cell 0; the second quad (cell 1) as instances 2 and 3 (cells 3 and 4); cell 0 as instance 6
        const std::vector<MultiDrawCommand> indexedList = {
            {6, 1, 6, 0, 0}, {6, 2, 6, 4, 2}, {6, 1, 6, 0, 6}};
        const int indirectBefore = device->frameCounters().multiDrawIndirectCalls;
        check(drawList(indexedVertices, indices, indexedList) == "#..##.#.",
            "the first index, base vertex, instances and first instance of each sub-draw: cells 0, 3, 4 and 6");
        const int indirectCalls = device->frameCounters().multiDrawIndirectCalls - indirectBefore;
        check(device->supportsMultiDrawIndirect() ? indirectCalls == 1 : indirectCalls == 0,
            "issued as " + std::to_string(indirectCalls) + " indirect draw call(s), as the device says it can");
        check(drawList(indexedVertices, indices, {{6, 1, 0, 0, 0}}) == "........",
            "a sub-draw of the degenerate first six indices draws nothing");
        check(drawList(indexedVertices, indices, {{6, 3, 6, 4, 4}}) == ".....###",
            "a list of one: the second quad as instances 4..6, cells 5, 6 and 7");

        std::cout << "\na non-indexed list\n";
        // The two quads as triangle lists, six vertices each.
        std::vector<Vertex> triangles;
        for (const int cell : {0, 1}) {
            const auto quad = cellCorners(cell);
            for (const int corner : {0, 1, 2, 0, 2, 3}) {
                triangles.push_back(quad[static_cast<size_t>(corner)]);
            }
        }
        const auto listVertices = vertexBuffer(device.get(), triangles);
        const std::vector<MultiDrawCommand> list = {{6, 1, 0, 0, 0}, {6, 2, 6, 0, 2}, {6, 1, 0, 0, 6}};
        check(drawList(listVertices, nullptr, list) == "#..##.#.",
            "the first vertex, instances and first instance of each sub-draw: cells 0, 3, 4 and 6");
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("multi-draw gpu");
}
