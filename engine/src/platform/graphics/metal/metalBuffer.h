// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.07.2025.
//
#pragma once

#include <Metal/Metal.hpp>

#include "metalGraphicsDevice.h"
#include "platform/graphics/gpu.h"

namespace visutwin::canvas::gpu
{
    enum BufferUsage : uint32_t {
        MAP_READ = 0x0001,
        MAP_WRITE = 0x0002,
        COPY_SRC = 0x0004,
        COPY_DST = 0x0008,
        INDEX = 0x0010,
        VERTEX = 0x0020,
        UNIFORM = 0x0040,
        STORAGE = 0x0080,
        INDIRECT = 0x0100,
        QUERY_RESOLVE = 0x0200
    };

    /**
     * Metal implementation of a GPU buffer.
     * Wraps MTL::Buffer and provides buffer management functionality.
     */
    class MetalBuffer : public HardwareBuffer {
    public:
        explicit MetalBuffer(const BufferUsage usageFlags) : _usageFlags(usageFlags) {}
        ~MetalBuffer() override;

        void allocate(MTL::Device* device, size_t size);

        /**
         * Write data to a storage buffer
         *
         * @param bufferOffset - The offset into the buffer to write to
         * @param data - The source data
         * @param dataSize - The number of bytes to write
         */
        void write(size_t bufferOffset, const void* data, size_t dataSize) const;

        [[nodiscard]] size_t size() const {
            return _buffer ? _buffer->length() : 0;
        }

        [[nodiscard]] MTL::Buffer* raw() const {
            return _buffer;
        }

        // HardwareBuffer interface
        void upload(GraphicsDevice* device, const void* data, size_t size) override;
        void* nativeHandle() const override { return _buffer; }

        // Unlock the buffer and upload data to GPU
        void unlock(MetalGraphicsDevice* device, const std::vector<uint8_t>& storage);

        // Allocate the buffer with the specified size
        void allocate(MetalGraphicsDevice* device, size_t size);

        /// Adopt a pre-existing MTL::Buffer, taking ownership via retain/release.
        /// Used for zero-copy paths where the buffer was allocated externally
        /// (e.g., GPU compute output buffers).
        void adoptBuffer(MTL::Buffer* buffer);

    private:
        BufferUsage _usageFlags;
        MTL::Buffer* _buffer = nullptr;
    };
}
