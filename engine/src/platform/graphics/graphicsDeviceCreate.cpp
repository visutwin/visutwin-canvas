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

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    std::unique_ptr<GraphicsDevice> createGraphicsDevice(const GraphicsDeviceOptions& options)
    {
        switch (options.backend)
        {
        case Backend::Metal:
#ifdef VISUTWIN_HAS_METAL
            return std::make_unique<MetalGraphicsDevice>(options);
#else
            spdlog::error("Metal backend not available on this platform");
            return nullptr;
#endif
        case Backend::Vulkan:
#ifdef VISUTWIN_HAS_VULKAN
            return std::make_unique<VulkanGraphicsDevice>(options);
#else
            spdlog::error("Vulkan backend not available on this platform");
            return nullptr;
#endif
        default:
            spdlog::error("Unknown backend: {}", static_cast<int>(options.backend));
            return nullptr;
        }
    }
}
