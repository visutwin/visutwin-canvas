// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// One Slang module drawn on a real device of this build's backend through
// GraphicsDevice::createShaderFromCode: compiled to MSL for Metal, to SPIR-V for
// Vulkan, by the same code. It holds the three things phase 0 of the single-source
// migration has to prove on hardware:
//   - the matrix layout: the vertex stage applies `mul(params.model, p)` with a
//     translation of +1 in x, which must shift the quad's oversized triangle onto the
//     RIGHT half of the target and leave the left half cleared. Read row-major, the
//     translation lands in the w row and the triangle covers something else;
//   - the variant: the fragment picks its colour from feature word 0, bit 0, which the
//     engine supplies as a function constant (Metal) or a specialization constant
//     (Vulkan) from ShaderDefinition::features, so ONE compiled library draws both
//     colours;
//   - the quad path's bindings: the uniform block at Metal buffer 3 / Vulkan set 0
//     binding 0, declared once with register(b3) and [[vk::binding(0, 0)]].
// It needs a GPU and a window, so it carries the `gpu` label.
//
// The metal-cpp *_PRIVATE_IMPLEMENTATION translation unit of this executable: a program
// linking the static engine supplies it (see examples/exampleApp.cpp).
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <QuartzCore/QuartzCore.hpp>
#endif

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"
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
    constexpr const char* kModule = R"(
struct Params { float4x4 model; float4 colorA; float4 colorB; };
[[vk::binding(0, 0)]] ConstantBuffer<Params> params : register(b3);
[[vk::constant_id(0)]] const uint vtFeatureMask0 = 0;

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
    o.clip = mul(params.model, float4(i.position, 1.0));
    o.uv = i.uv;
    return o;
}

[shader("fragment")]
float4 psMain(VSOut i) : SV_Target0
{
    return (vtFeatureMask0 & 1u) != 0u ? params.colorA : params.colorB;
}
)";

    struct Params
    {
        float model[16];
        float colorA[4];
        float colorB[4];
    };
    static_assert(sizeof(Params) == 96);

    constexpr uint32_t kSize = 64;

    struct Pixel
    {
        uint8_t r, g, b, a;
    };

    Pixel at(const std::vector<uint8_t>& pixels, const uint32_t x, const uint32_t y)
    {
        const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
        return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
    }

    bool isColor(const Pixel p, const uint8_t r, const uint8_t g, const uint8_t b)
    {
        auto close = [](const uint8_t a, const uint8_t c) { return (a > c ? a - c : c - a) <= 2; };
        return close(p.r, r) && close(p.g, g) && close(p.b, b);
    }

    std::string describe(const Pixel p)
    {
        return "(" + std::to_string(p.r) + ", " + std::to_string(p.g) + ", " + std::to_string(p.b) + ")";
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
    SDL_Window* window = SDL_CreateWindow("slang-gpu", 64, 64, flags);
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

        // One Slang module, compiled for this device's backend.
        SlangCompileRequest request;
        request.moduleName = "slang-gpu-test";
        request.source = kModule;
        request.entryPoints = {"vsMain", "psMain"};
        request.target = device->shaderLanguage() == ShaderLanguage::Glsl ? SlangTarget::Spirv : SlangTarget::Msl;
        const auto compiled = SlangCompiler::compile(request);
        std::cout << "  slang -> " << (request.target == SlangTarget::Spirv ? "SPIR-V" : "MSL") << ": "
                  << compiled.seconds * 1000.0 << " ms\n";
        if (!check(compiled.ok, "the module compiles for " + std::string(backendName(backend)) + ": " + compiled.diagnostics)) {
            return finish("slang gpu");
        }

        ShaderCode code;
        if (request.target == SlangTarget::Spirv) {
            code.vertexSpirv = compiled.spirv(0);
            code.fragmentSpirv = compiled.spirv(1);
        } else {
            code.metalSource = compiled.programText();
        }

        // The colour target and its pass.
        TextureOptions colorOptions{};
        colorOptions.name = "slang-gpu-color";
        colorOptions.width = kSize;
        colorOptions.height = kSize;
        colorOptions.mipmaps = false;
        auto color = std::make_unique<Texture>(device.get(), colorOptions);
        RenderTargetOptions targetOptions{};
        targetOptions.graphicsDevice = device.get();
        targetOptions.colorBuffer = color.get();
        targetOptions.name = "slang-gpu-target";
        auto target = device->createRenderTarget(targetOptions);

        const Color clearColor(0.0f, 0.0f, 0.0f, 1.0f);
        Params params{};
        Matrix4::translation(Vector3(1.0f, 0.0f, 0.0f)).store(params.model);
        params.colorA[0] = 1.0f; params.colorA[3] = 1.0f;   // red when feature bit 0 is set
        params.colorB[1] = 1.0f; params.colorB[3] = 1.0f;   // green otherwise

        // Two variants of ONE compiled program: the feature set differs, the code does not.
        for (int variant = 0; variant < 2; ++variant) {
            ShaderDefinition definition{};
            definition.name = variant ? "slang-gpu-variant-on" : "slang-gpu-variant-off";
            definition.vshader = "vsMain";
            definition.fshader = "psMain";
            if (variant) {
                definition.features.set(ShaderFeature::BaseColorMap); // word 0, bit 0
            }
            const auto created = std::chrono::steady_clock::now();
            auto shader = device->createShaderFromCode(definition, code);
            if (!check(shader != nullptr, definition.name + ": the shader is created from code")) {
                continue;
            }

            RenderPass pass(sharedDevice);
            pass.init(target);
            pass.setClearColor(&clearColor);
            // Nothing outside the frame graph sets the draw state: a bake or a test does.
            device->setBlendState(BlendState::noBlend());
            device->setDepthState(DepthState::noDepth());
            device->setCullMode(CullMode::CULLFACE_NONE);
            device->frameStart();
            device->startRenderPass(&pass);
            {
                QuadRender quad(shader);
                quad.setUniforms(params);
                quad.render();   // the first draw creates the pipeline (and on Metal waits for the compile)
            }
            device->endRenderPass(&pass);
            device->frameEnd();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - created).count();
            std::cout << "  " << definition.name << ": create shader + first draw " << ms << " ms\n";

            std::vector<uint8_t> pixels;
            if (!check(color->read(pixels) && pixels.size() == static_cast<size_t>(kSize) * kSize * 4,
                    definition.name + ": the target reads back")) {
                continue;
            }
            const Pixel left = at(pixels, kSize / 4, kSize / 2);
            const Pixel right = at(pixels, (3 * kSize) / 4, kSize / 2);
            const Pixel topRight = at(pixels, (3 * kSize) / 4, kSize / 8);
            const Pixel bottomRight = at(pixels, (3 * kSize) / 4, (7 * kSize) / 8);
            check(isColor(left, 0, 0, 0), definition.name + ": the left half is left cleared " + describe(left) +
                " (the translation column of the matrix moved the triangle, so mul reads column-major)");
            const uint8_t r = variant ? 255 : 0;
            const uint8_t g = variant ? 0 : 255;
            check(isColor(right, r, g, 0), definition.name + ": the right half carries the variant's colour " +
                describe(right));
            check(isColor(topRight, r, g, 0) && isColor(bottomRight, r, g, 0),
                definition.name + ": the whole right half is covered " + describe(topRight) + " " + describe(bottomRight));
        }
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("slang gpu");
}
