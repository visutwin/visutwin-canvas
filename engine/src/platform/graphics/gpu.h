// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.09.2025
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;
}

namespace visutwin::canvas::gpu
{
    /// What a non-blocking readback hands back: the region's pixels, tightly packed, valid
    /// during the call only.
    using TextureReadCallback = std::function<void(const uint8_t* data, size_t size)>;

    /// The rectangle of one mip level and one face a readback covers.
    struct TextureReadRegion
    {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevel = 0;
        // A cubemap face or an array layer; a volume's FIRST depth slice.
        uint32_t face = 0;
        // The depth slices of a volume read from `face` on, packed slice after slice; 1
        // for any other texture.
        uint32_t depth = 1;
    };

    // A copy between two textures of one format: a width x height block of the source's
    // mip level at (sourceX, sourceY) to the destination's mip level at (destX, destY), on
    // the same cube face, array layer or volume depth slices (0 for a 2D texture).
    struct TextureCopyRegion
    {
        uint32_t sourceX = 0;
        uint32_t sourceY = 0;
        uint32_t destX = 0;
        uint32_t destY = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t sourceMipLevel = 0;
        uint32_t destMipLevel = 0;
        // A cubemap face or an array layer; a volume's first depth slice, the same on both sides.
        uint32_t face = 0;
        // The depth slices of a volume copied from `face` on; 1 for any other texture.
        uint32_t depth = 1;
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

        /**
         * Copy `region` of `source`, a texture of the same backend and format, into this
         * one on the GPU. Call it through `Texture::copy`, which validates the region.
         * Returns false when the backend cannot do it now.
         */
        /**
         * Queues a NON-blocking read of `region`: the backend copies it into staging at the end
         * of the frame it is queued in, ordered after everything that frame rendered, and calls
         * `callback` on the device's thread at a later frame start, once the GPU has finished
         * that frame. Returns false when the backend cannot.
         */
        virtual bool readAsync(GraphicsDevice* /*device*/, const TextureReadRegion& /*region*/,
            TextureReadCallback /*callback*/)
        {
            return false;
        }

        virtual bool copyFrom(GraphicsDevice* /*device*/, HardwareTexture& /*source*/,
            const TextureCopyRegion& /*region*/)
        {
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
