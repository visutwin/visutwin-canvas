// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include <memory>

#include "graphicsDevice.h"
#include "SDL3/SDL_video.h"

namespace visutwin::canvas
{
    enum class Backend
    {
        Metal,
        Vulkan,
        WebGPU
    };

    struct GraphicsDeviceOptions
    {
        Backend backend{ Backend::Metal };

        void* swapChain{nullptr};

        SDL_Window* window{nullptr};
    };

    std::unique_ptr<GraphicsDevice> createGraphicsDevice(const GraphicsDeviceOptions& options);
}
