// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Backend naming and the window-title tag.
//
// Deliberately a separate translation unit from graphicsDeviceCreate.cpp: that
// one references MetalGraphicsDevice / VulkanGraphicsDevice, so anything using
// it links in every compiled-in backend. Keeping these two functions apart lets
// a Vulkan-only target (tests/vulkanSmoke.cpp) use them without pulling in the
// Metal objects, which need the metal-cpp *_PRIVATE_IMPLEMENTATION TU.
//
#include "graphicsDeviceCreate.h"

#include <string>

namespace visutwin::canvas
{
    const char* backendName(const Backend backend)
    {
        switch (backend) {
            case Backend::Metal:  return "Metal";
            case Backend::Vulkan: return "Vulkan";
            case Backend::WebGPU: return "WebGPU";
        }
        return "Unknown";
    }

    void applyBackendWindowTitle(SDL_Window* window, const Backend backend)
    {
        if (!window) {
            return;
        }
        const char* current = SDL_GetWindowTitle(window);
        std::string title = current ? current : "";

        // Strip a tag left by a previous backend so switching does not stack
        // them. Only a KNOWN backend name is stripped, so a title that
        // legitimately ends in brackets — "Example [WIP]" — is left alone.
        if (!title.empty() && title.back() == ']') {
            const auto open = title.rfind(" [");
            if (open != std::string::npos) {
                const auto inner =
                    title.substr(open + 2, title.size() - open - 3);
                for (const auto candidate :
                        {Backend::Metal, Backend::Vulkan, Backend::WebGPU}) {
                    if (inner == backendName(candidate)) {
                        title.erase(open);
                        break;
                    }
                }
            }
        }

        title += " [";
        title += backendName(backend);
        title += "]";
        SDL_SetWindowTitle(window, title.c_str());
    }
}
