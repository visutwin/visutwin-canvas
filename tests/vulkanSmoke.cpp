// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>

#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/vulkan/vulkanGraphicsDevice.h"
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
        for (int frame = 0; frame < 3 && result == 0; ++frame) {
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
