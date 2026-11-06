// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.11.2026
//
// EXPERIMENTAL bindless table (platform/graphics/bindlessTable.h) on a real device of this
// build's backend: 64 two-by-two textures and 4 samplers in ONE table, read by a Slang
// program that picks texture k and sampler k % 4 by a run-time index — per PIXEL (each
// 8x8 cell of the target reads its own entry, a divergent index) and per DRAW (a uniform
// index over the whole target). On Metal the table is an argument buffer of GPU resource
// ids at buffer slot 13; on Vulkan an update-after-bind descriptor set 7. Every cell must
// come back with its texture's red and the quadrant its sampler's wrap mode selects:
//   sampler 0 clamp   at uv (1.25, 2.25) -> texel (1, 1): green 255, blue 255
//   sampler 1 repeat  at uv (1.25, 2.25) -> texel (0, 0): green 0,   blue 0
//   sampler 2 mirror  at uv (1.25, 2.25) -> texel (1, 0): green 255, blue 0
//   sampler 3 linear  at uv (0.5, 0.5)   -> the mean of the four texels: 127 or 128
// A wrong texture index shows in red, a wrong sampler index in green and blue. The device
// reports the limits it found at creation (the spdlog lines), which the report in
// ARCHITECTURE.md (design directions, item 2) is written from. Without support the test
// says so and passes: the point is the report, and a device without descriptor indexing
// or argument buffers tier 2 is a finding, not a failure.
//
// It needs a GPU and a window, so it carries the `gpu` label. The metal-cpp
// *_PRIVATE_IMPLEMENTATION translation unit of this executable, as slangGpuTests.cpp.
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <QuartzCore/QuartzCore.hpp>
#endif

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/math/color.h"
#include "platform/graphics/bindlessTable.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/slangCompiler.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/quadRender.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // The table's declaration is the contract in bindlessTable.h: samplers first, sized
    // arrays, VT_NONUNIFORM at the indexing expression. TEXN / SAMN come from the device.
    constexpr const char* kModule = R"(
#if defined(VT_TARGET_METAL)
#define VT_NONUNIFORM(k) (k)
#else
#define VT_NONUNIFORM(k) NonUniformResourceIndex(k)
#endif

struct Params { uint4 index; };   // x: the uniform index, y: 1 to use it, else per cell
[[vk::binding(0, 0)]] ConstantBuffer<Params> params : register(b3);

struct Table { SamplerState samplers[SAMN]; Texture2D textures[TEXN]; };
[[vk::binding(0, 7)]] ParameterBlock<Table> table : register(b13);

struct VSIn {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};
struct VSOut { float4 clip : SV_Position; float2 uv : TEXCOORD0; };

[shader("vertex")]
VSOut vsMain(VSIn i)
{
    VSOut o;
    o.clip = float4(i.position.xy, 0.0, 1.0);
    o.uv = i.uv;
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut i) : SV_Target0
{
    uint k;
    if (params.index.y != 0u) {
        k = params.index.x;
    } else {
        uint2 cell = uint2(i.clip.xy) / 8u;
        k = cell.y * 8u + cell.x;
    }
    uint s = k % 4u;
    float2 uv = s == 3u ? float2(0.5, 0.5) : float2(1.25, 2.25);
    return table.textures[VT_NONUNIFORM(k)].Sample(table.samplers[VT_NONUNIFORM(s)], uv);
}
)";

    struct Params
    {
        uint32_t index[4];
    };

    constexpr uint32_t kSize = 64;
    constexpr uint32_t kTextures = 64;
    constexpr uint32_t kSamplers = 4;

    struct Pixel
    {
        uint8_t r, g, b, a;
    };

    Pixel at(const std::vector<uint8_t>& pixels, const uint32_t x, const uint32_t y)
    {
        const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
        return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
    }

    std::string describe(const Pixel p)
    {
        return "(" + std::to_string(p.r) + ", " + std::to_string(p.g) + ", " + std::to_string(p.b) + ")";
    }

    uint8_t redOf(const uint32_t k) { return static_cast<uint8_t>(k * 3 + 2); }

    // What texture k read through sampler k % 4 must return.
    Pixel expected(const uint32_t k)
    {
        switch (k % kSamplers) {
        case 0: return {redOf(k), 255, 255, 255};
        case 1: return {redOf(k), 0, 0, 255};
        case 2: return {redOf(k), 255, 0, 255};
        default: return {redOf(k), 127, 127, 255};
        }
    }

    bool matches(const Pixel p, const Pixel e)
    {
        auto close = [](const uint8_t a, const uint8_t c) { return (a > c ? a - c : c - a) <= 2; };
        return close(p.r, e.r) && close(p.g, e.g) && close(p.b, e.b);
    }

    std::unique_ptr<Texture> makeEntry(GraphicsDevice* device, const uint32_t k)
    {
        TextureOptions options;
        options.name = "bindless-" + std::to_string(k);
        options.width = 2;
        options.height = 2;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;
        auto texture = std::make_unique<Texture>(device, options);
        // Texel (x, y): red names the texture, green x, blue y; row 0 is the top.
        uint8_t pixels[16];
        for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t x = 0; x < 2; ++x) {
                uint8_t* p = pixels + (y * 2 + x) * 4;
                p[0] = redOf(k);
                p[1] = static_cast<uint8_t>(x * 255);
                p[2] = static_cast<uint8_t>(y * 255);
                p[3] = 255;
            }
        }
        texture->setLevelData(0, pixels, sizeof(pixels));
        texture->upload();
        return texture;
    }

    TextureSamplerState sampler(const AddressMode mode, const TextureSamplerState::Filter filter)
    {
        TextureSamplerState state;
        state.addressU = mode;
        state.addressV = mode;
        state.addressW = mode;
        state.minFilter = filter;
        state.magFilter = filter;
        state.mipFilter = TextureSamplerState::Filter::Nearest;
        state.maxAnisotropy = 1.0f;
        return state;
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
    SDL_Window* window = SDL_CreateWindow("bindless-gpu", 64, 64, flags);
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
        auto sharedDevice = std::shared_ptr<GraphicsDevice>(device.get(), [](GraphicsDevice*) {});

        std::cout << "  bindless textures: " << (device->supportsBindlessTextures() ? "supported" : "unsupported")
                  << ", capacity " << device->bindlessTextureCapacity() << " textures, "
                  << device->bindlessSamplerCapacity() << " samplers\n";
        if (!device->supportsBindlessTextures()) {
            check(true, "no bindless support on this device: the mechanism is not tested here (see the device log)");
            return finish("bindless gpu");
        }
        check(device->bindlessTextureCapacity() >= kTextures && device->bindlessSamplerCapacity() >= kSamplers,
            "the device's capacities hold the test's table");

        // The program, sized to the device's table.
        SlangCompileRequest request;
        request.moduleName = "bindless-gpu-test";
        request.source = kModule;
        request.entryPoints = {"vsMain", "fsMain"};
        request.target = device->shaderLanguage() == ShaderLanguage::Glsl ? SlangTarget::Spirv : SlangTarget::Msl;
        request.defines = {{"TEXN", std::to_string(device->bindlessTextureCapacity())},
            {"SAMN", std::to_string(device->bindlessSamplerCapacity())}};
        const auto compiled = SlangCompiler::compile(request);
        if (!check(compiled.ok, "the program compiles for " + std::string(backendName(backend)) + ": " + compiled.diagnostics)) {
            return finish("bindless gpu");
        }
        ShaderCode code;
        if (request.target == SlangTarget::Spirv) {
            code.vertexSpirv = compiled.spirv(0);
            code.fragmentSpirv = compiled.spirv(1);
        } else {
            code.metalSource = compiled.programText();
        }
        ShaderDefinition definition{};
        definition.name = "bindless-gpu";
        definition.vshader = "vsMain";
        definition.fshader = "fsMain";
        auto shader = device->createShaderFromCode(definition, code);
        if (!check(shader != nullptr, "the shader is created from code")) {
            return finish("bindless gpu");
        }

        // The table: 64 textures, 4 samplers.
        std::vector<std::unique_ptr<Texture>> textures;
        BindlessTableDesc desc;
        for (uint32_t k = 0; k < kTextures; ++k) {
            textures.push_back(makeEntry(device.get(), k));
            desc.textures.push_back(textures.back().get());
        }
        desc.samplers = {
            sampler(ADDRESS_CLAMP_TO_EDGE, TextureSamplerState::Filter::Nearest),
            sampler(ADDRESS_REPEAT, TextureSamplerState::Filter::Nearest),
            sampler(ADDRESS_MIRRORED_REPEAT, TextureSamplerState::Filter::Nearest),
            sampler(ADDRESS_CLAMP_TO_EDGE, TextureSamplerState::Filter::Linear),
        };
        auto table = device->createBindlessTable(desc);
        if (!check(table != nullptr, "the table is created")) {
            return finish("bindless gpu");
        }

        // The target.
        TextureOptions colorOptions{};
        colorOptions.name = "bindless-gpu-color";
        colorOptions.width = kSize;
        colorOptions.height = kSize;
        colorOptions.mipmaps = false;
        auto color = std::make_unique<Texture>(device.get(), colorOptions);
        RenderTargetOptions targetOptions{};
        targetOptions.graphicsDevice = device.get();
        targetOptions.colorBuffer = color.get();
        targetOptions.name = "bindless-gpu-target";
        auto target = device->createRenderTarget(targetOptions);
        const Color clearColor(0.0f, 0.0f, 0.0f, 1.0f);

        const auto render = [&](const Params& params) {
            RenderPass pass(sharedDevice);
            pass.init(target);
            pass.setClearColor(&clearColor);
            device->setBlendState(BlendState::noBlend());
            device->setDepthState(DepthState::noDepth());
            device->setCullMode(CullMode::CULLFACE_NONE);
            device->frameStart();
            device->setBindlessTable(table.get());
            device->startRenderPass(&pass);
            {
                QuadRender quad(shader);
                quad.setUniforms(params);
                quad.render();
            }
            device->endRenderPass(&pass);
            device->setBindlessTable(nullptr);
            device->frameEnd();
            std::vector<uint8_t> pixels;
            check(color->read(pixels) && pixels.size() == static_cast<size_t>(kSize) * kSize * 4, "the target reads back");
            return pixels;
        };

        // Per pixel: every cell its own entry, a divergent index.
        {
            const std::vector<uint8_t> pixels = render(Params{{0, 0, 0, 0}});
            int wrong = 0;
            std::string firstWrong;
            for (uint32_t k = 0; k < kTextures; ++k) {
                const uint32_t x = (k % 8) * 8 + 4;
                const uint32_t y = (k / 8) * 8 + 4;
                const Pixel p = at(pixels, x, y);
                if (!matches(p, expected(k))) {
                    if (wrong == 0) {
                        firstWrong = "entry " + std::to_string(k) + " read " + describe(p) + ", expected " + describe(expected(k));
                    }
                    ++wrong;
                }
            }
            check(wrong == 0, "every cell reads its own texture through its own sampler by a per-pixel index"
                + (wrong ? " (" + std::to_string(wrong) + " wrong, first: " + firstWrong + ")" : std::string()));
        }

        // Per draw: one uniform index over the whole target, for two entries.
        for (const uint32_t k : {5u, 63u}) {
            const std::vector<uint8_t> pixels = render(Params{{k, 1, 0, 0}});
            const Pixel centre = at(pixels, kSize / 2, kSize / 2);
            const Pixel corner = at(pixels, 1, kSize - 2);
            check(matches(centre, expected(k)) && matches(corner, expected(k)),
                "a uniform index " + std::to_string(k) + " reads entry " + std::to_string(k) + " everywhere: "
                + describe(centre) + " " + describe(corner) + ", expected " + describe(expected(k)));
        }

        table.reset();
        textures.clear();
        target.reset();
        color.reset();
        shader.reset();
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("bindless gpu");
}
