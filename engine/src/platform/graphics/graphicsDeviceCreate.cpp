// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#include "graphicsDeviceCreate.h"

#ifdef VISUTWIN_HAS_METAL
#include "metal/metalGraphicsDevice.h"
#endif

#ifdef VISUTWIN_HAS_VULKAN
#include "vulkan/vulkanGraphicsDevice.h"
#endif

#include <cstdlib>
#include <cstring>
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        // VISUTWIN_BACKEND env var overrides the caller-supplied backend.
        // Useful for switching between Metal and Vulkan on macOS without
        // having to recompile every example.  Recognised values
        // (case-sensitive): "metal", "vulkan".
        Backend applyEnvOverride(Backend requested)
        {
            const char* env = std::getenv("VISUTWIN_BACKEND");
            if (!env || env[0] == '\0') return requested;
            if (std::strcmp(env, "metal") == 0)   return Backend::Metal;
            if (std::strcmp(env, "vulkan") == 0)  return Backend::Vulkan;
            spdlog::warn("VISUTWIN_BACKEND='{}' not recognised — using requested backend", env);
            return requested;
        }

    }

    std::unique_ptr<GraphicsDevice> createGraphicsDevice(const GraphicsDeviceOptions& options)
    {
        const Backend backend = applyEnvOverride(options.backend);
        if (backend != options.backend) {
            spdlog::info("Backend overridden by VISUTWIN_BACKEND env var: {} → {}",
                backendName(options.backend), backendName(backend));
        }

        std::unique_ptr<GraphicsDevice> device;
        switch (backend)
        {
        case Backend::Metal:
#ifdef VISUTWIN_HAS_METAL
            device = std::make_unique<MetalGraphicsDevice>(options);
            break;
#else
            spdlog::error("Metal backend not available on this platform");
            return nullptr;
#endif
        case Backend::Vulkan:
#ifdef VISUTWIN_HAS_VULKAN
            device = std::make_unique<VulkanGraphicsDevice>(options);
            break;
#else
            spdlog::error("Vulkan backend not available on this platform");
            return nullptr;
#endif
        default:
            spdlog::error("Unknown backend: {}", backendName(backend));
            return nullptr;
        }

        // Only after construction succeeded — a device that threw or returned
        // null must not leave a misleading tag on the window.
        if (device && options.window) {
            applyBackendWindowTitle(options.window, backend);
        }
        return device;
    }
}
