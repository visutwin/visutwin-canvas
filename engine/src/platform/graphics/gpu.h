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
    /// The rectangle of one mip level and one face a readback covers.
    /// Upstream's `Texture#read(x, y, width, height, {mipLevel, face})`, as a
    /// struct rather than six positional arguments.
    struct TextureReadRegion
    {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevel = 0;
        uint32_t face = 0;
    };

    class HardwareTexture
    {
    public:
        virtual ~HardwareTexture() = default;

        // Upload texture data immediately if needed
        virtual void uploadImmediate(GraphicsDevice* device) = 0;

        virtual void propertyChanged(uint32_t flag) = 0;

        /**
         * Copy `region` out of the GPU texture into `out`, tightly packed at the
         * texture's bytes-per-pixel, and BLOCK until it has arrived. Call it
         * through `Texture::read`, which validates the region and sizes `out`.
         *
         * Both backends go through a staging resource rather than reading the
         * texture's own memory: the interesting textures to read are render
         * targets, which are device-private on Apple Silicon and in
         * TRANSFER-unfriendly layouts under Vulkan. Reading one directly returns
         * whatever happened to be mapped, which is not an error and not blank —
         * it is plausible garbage, and it is what `tools/generate-env-atlas`
         * produced for as long as it existed.
         *
         * Returns false when the backend cannot do it, leaving `out` untouched.
         */
        virtual bool read(GraphicsDevice* device, const TextureReadRegion& region,
            uint8_t* out, size_t outSize)
        {
            (void)device; (void)region; (void)out; (void)outSize;
            return false;
        }
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
