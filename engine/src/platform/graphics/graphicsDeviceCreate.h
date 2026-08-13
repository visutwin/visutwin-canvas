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

        /// Sync presentation to the display refresh (Metal: CAMetalLayer
        /// displaySyncEnabled + a 2-deep drawable queue). Keeps the frame
        /// loop's measured dt even, which animations need to look smooth.
        /// Disable only for uncapped-fps benchmarking.
        bool vsync{true};

#ifdef VISUTWIN_DEBUG_GPU_VALIDATION
        bool enableValidation{true};
#else
        bool enableValidation{false};
#endif
    };

    /// Human-readable backend name ("Metal", "Vulkan", "WebGPU").
    const char* backendName(Backend backend);

    /// Appends " [Metal]" / " [Vulkan]" to the window title. Idempotent: a tag
    /// left by a previous backend is replaced rather than stacked, and only a
    /// suffix naming a known backend is stripped, so "Example [WIP]" survives.
    /// createGraphicsDevice calls this; exposed so it can be tested and so an
    /// app can re-apply it after changing its own title.
    void applyBackendWindowTitle(SDL_Window* window, Backend backend);

    /// Creates the device and, when options.window is set, tags the window
    /// title with the backend actually used — e.g. "My Example [Vulkan]".
    /// The backend can differ from the one requested (VISUTWIN_BACKEND), so
    /// the title is the quickest way to tell which one a window is running.
    std::unique_ptr<GraphicsDevice> createGraphicsDevice(const GraphicsDeviceOptions& options);
}
