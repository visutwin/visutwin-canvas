// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <exception>
#include <memory>
#include <vector>

#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/stencilParameters.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "platform/graphics/vulkan/vulkanGraphicsDevice.h"
#include "scene/mesh.h"
#include "spdlog/spdlog.h"

using namespace visutwin::canvas;

int main()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        spdlog::error("Vulkan smoke: SDL_Init failed: {}", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "VisuTwin Vulkan validation smoke",
        64,
        64,
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) {
        spdlog::error("Vulkan smoke: SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    int result = 0;
    std::shared_ptr<const std::atomic_uint32_t> validationErrors;
    try {
        GraphicsDeviceOptions options{};
        options.backend = Backend::Vulkan;
        options.window = window;
        options.enableValidation = true;

        auto device = std::make_unique<VulkanGraphicsDevice>(options);
        if (!device->validationEnabled()) {
            spdlog::error("Vulkan smoke: validation was requested but is disabled");
            result = 1;
        }

        validationErrors = device->validationErrorCounter();

        // Draw a triangle into a stencil-capable target while changing every
        // core pipeline state represented by the cross-platform interface.
        // This catches invalid pipeline/rendering compatibility as well as
        // missing dynamic stencil-reference commands.
        {
            TextureOptions colorOptions{};
            colorOptions.name = "vulkan-smoke-color";
            colorOptions.width = 64;
            colorOptions.height = 64;
            colorOptions.mipmaps = false;
            auto color = std::make_unique<Texture>(device.get(), colorOptions);

            RenderTargetOptions targetOptions{};
            targetOptions.graphicsDevice = device.get();
            targetOptions.colorBuffer = color.get();
            targetOptions.depth = true;
            targetOptions.stencil = true;
            targetOptions.name = "vulkan-smoke-target";
            auto target = device->createRenderTarget(targetOptions);

            auto sharedDevice = std::shared_ptr<GraphicsDevice>(
                device.get(), [](GraphicsDevice*) {});
            RenderPass pass(sharedDevice);
            pass.init(target);
            const Color clearColor(0.05f, 0.1f, 0.2f, 1.0f);
            const float clearDepth = 1.0f;
            const int clearStencil = 0;
            pass.setClearColor(&clearColor);
            pass.setClearDepth(&clearDepth);
            pass.setClearStencil(&clearStencil);

            constexpr std::array<float, 42> vertices = {
                -0.5f, -0.5f, 0.0f,  0, 0, 1,  0, 0,  1, 0, 0, 1,  0, 0,
                 0.5f, -0.5f, 0.0f,  0, 0, 1,  1, 0,  1, 0, 0, 1,  0, 0,
                 0.0f,  0.5f, 0.0f,  0, 0, 1,  0.5f, 1,  1, 0, 0, 1,  0, 0
            };
            VertexBufferOptions vertexOptions{};
            vertexOptions.data.resize(sizeof(vertices));
            std::memcpy(vertexOptions.data.data(), vertices.data(), sizeof(vertices));
            auto format = std::make_shared<VertexFormat>(14 * sizeof(float));
            auto vertexBuffer = device->createVertexBuffer(format, 3, vertexOptions);
            auto shader = device->createShader(ShaderDefinition{.name = "vulkan-smoke"});

            Primitive triangle{};
            triangle.type = PRIMITIVE_TRIANGLES;
            triangle.count = 3;

            auto maskedBlend = std::make_shared<BlendState>();
            maskedBlend->setGreenWrite(false);
            maskedBlend->setBlueWrite(false);
            maskedBlend->setAlphaWrite(false);
            auto defaultDepth = std::make_shared<DepthState>();

            auto stencilFront = std::make_shared<StencilParameters>();
            stencilFront->setCompareFunction(StencilCompareFunction::Always);
            stencilFront->setPassOperation(StencilOperation::Replace);
            stencilFront->setReference(1);
            stencilFront->setReadMask(0x0f);
            stencilFront->setWriteMask(0x0f);

            auto stencilBack = std::make_shared<StencilParameters>();
            stencilBack->setCompareFunction(StencilCompareFunction::NotEqual);
            stencilBack->setFailOperation(StencilOperation::IncrementWrap);
            stencilBack->setDepthFailOperation(StencilOperation::DecrementClamp);
            stencilBack->setPassOperation(StencilOperation::Invert);
            stencilBack->setReference(2);
            stencilBack->setReadMask(0xf0);
            stencilBack->setWriteMask(0xf0);

            device->frameStart();
            device->startRenderPass(&pass);
            device->setViewport(0, 0, 64, 64);
            // Deliberately exceeds the target to verify Vulkan scissor clamping.
            device->setScissor(-4, -4, 80, 80);
            device->setShader(shader);
            device->setVertexBuffer(vertexBuffer);
            device->setTransformUniforms(Matrix4::identity(), Matrix4::identity());
            device->setBlendState(maskedBlend);
            device->setDepthState(defaultDepth);
            device->setDepthBias(0.25f, 0.5f, 0.0f);
            device->setCullMode(CullMode::CULLFACE_BACK);
            device->setStencilState(stencilFront, stencilBack);
            device->draw(triangle);

            // Reference is dynamic and must not require a new pipeline.
            stencilFront->setReference(3);
            stencilBack->setReference(4);
            device->draw(triangle);

            auto alphaBlend = std::make_shared<BlendState>(BlendState::alphaBlend());
            auto noDepth = std::make_shared<DepthState>();
            noDepth->setDepthTest(false);
            noDepth->setDepthWrite(false);
            device->setBlendState(alphaBlend);
            device->setDepthState(noDepth);
            device->setDepthBias(0.0f, 0.0f, 0.0f);
            device->setCullMode(CullMode::CULLFACE_FRONT);
            device->setStencilState();
            device->draw(triangle);

            device->setCullMode(CullMode::CULLFACE_FRONTANDBACK);
            device->draw(triangle);
            device->endRenderPass(&pass);
            device->frameEnd();

            TextureOptions depthOptions{};
            depthOptions.name = "vulkan-smoke-depth";
            depthOptions.width = 64;
            depthOptions.height = 64;
            depthOptions.format = PixelFormat::PIXELFORMAT_DEPTH;
            depthOptions.mipmaps = false;
            auto depth = std::make_unique<Texture>(device.get(), depthOptions);

            RenderTargetOptions depthTargetOptions{};
            depthTargetOptions.graphicsDevice = device.get();
            depthTargetOptions.depthBuffer = depth.get();
            depthTargetOptions.name = "vulkan-smoke-depth-only";
            auto depthTarget = device->createRenderTarget(depthTargetOptions);
            RenderPass depthPass(sharedDevice);
            depthPass.init(depthTarget);
            depthPass.setClearDepth(&clearDepth);

            // Depth-only pipelines must preserve the requested cull state
            // instead of silently forcing double-sided rendering.
            device->frameStart();
            device->startRenderPass(&depthPass);
            device->setShader(shader);
            device->setVertexBuffer(vertexBuffer);
            device->setBlendState(maskedBlend);
            device->setDepthState(defaultDepth);
            device->setCullMode(CullMode::CULLFACE_FRONT);
            device->setStencilState();
            device->draw(triangle);
            device->endRenderPass(&depthPass);
            device->frameEnd();
        }

        for (int frame = 0; frame < 1 && result == 0; ++frame) {
            SDL_PumpEvents();
            device->frameStart();
            device->frameEnd();
        }

        if (vkDeviceWaitIdle(device->device()) != VK_SUCCESS) {
            spdlog::error("Vulkan smoke: vkDeviceWaitIdle failed");
            result = 1;
        }

        // Destroy the device before reading the final count so teardown
        // diagnostics emitted by the validation layer are failures too.
        device.reset();
    } catch (const std::exception& error) {
        spdlog::error("Vulkan smoke failed: {}", error.what());
        result = 1;
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    const uint32_t errorCount = validationErrors
        ? validationErrors->load(std::memory_order_relaxed)
        : 0;
    if (errorCount != 0) {
        spdlog::error("Vulkan smoke: {} validation error(s)", errorCount);
        return 1;
    }
    if (result != 0) {
        return result;
    }

    spdlog::info("Vulkan smoke passed with validation enabled and no errors");
    return 0;
}
