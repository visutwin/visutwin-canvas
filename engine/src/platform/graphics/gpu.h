// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.09.2025.
//
#pragma once

#include <cstddef>
#include <cstdint>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;
}

namespace visutwin::canvas::gpu
{
    class HardwareTexture
    {
    public:
        virtual ~HardwareTexture() = default;

        // Upload texture data immediately if needed
        virtual void uploadImmediate(GraphicsDevice* device) = 0;

        virtual void propertyChanged(uint32_t flag) = 0;
    };

    /**
     * Abstract base for GPU buffer objects.
     * Backend implementations (Metal, Vulkan) provide concrete allocation and upload logic.
     */
    class HardwareBuffer
    {
    public:
        virtual ~HardwareBuffer() = default;

        /// Upload data to the GPU buffer.
        virtual void upload(GraphicsDevice* device, const void* data, size_t size) = 0;

        /// Returns the backend-specific native handle (MTL::Buffer*, VkBuffer, etc.).
        virtual void* nativeHandle() const = 0;
    };
}
