// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "constants.h"
#include "vertexFormat.h"

namespace visutwin::canvas
{
    class GraphicsDevice;

    struct VertexBufferOptions
    {
        BufferUsage usage = BUFFER_STATIC;

        std::vector<uint8_t> data;
    };

    struct DeviceVRAM;

    /**
    * A vertex buffer is the mechanism via which the application specifies vertex data to the graphics hardware
     */
    class VertexBuffer
    {
    public:
        VertexBuffer(GraphicsDevice* graphicsDevice, std::shared_ptr<VertexFormat> format, int numVertices,
            const VertexBufferOptions& options = VertexBufferOptions{});

        virtual ~VertexBuffer();

        std::shared_ptr<VertexFormat> format() const { return _format; }

        // Copies data into vertex buffer's memor
        bool setData(const std::vector<uint8_t>& data);

        /**
         * Overwrites `size` bytes at byte `offset`, in the CPU copy and on the GPU, and
         * leaves the rest of the buffer alone: what a buffer shared by many small meshes
         * (UiGeometryArena) is filled through, where setData would re-send all of it.
         *
         * The caller owns the hazard any CPU write into a drawn buffer has: on Metal the
         * bytes land in memory the GPU reads directly, so a range that a frame still in
         * flight draws must not be rewritten (GraphicsDevice::maxFramesInFlight).
         * Returns false, writing nothing, when the range does not fit.
         */
        bool writeRange(size_t offset, const void* data, size_t size);

        // Notifies the graphics engine that the client side copy of the vertex buffer's memory can be
        // returned to the control of the graphics driver.
        virtual void unlock() = 0;

        int numVertices() const { return _numVertices; }
        /// Size of the GPU buffer in bytes (also for a zero-copy buffer, whose storage() is empty).
        int numBytes() const { return _numBytes; }
        BufferUsage usage() const { return _usage; }

        virtual void* nativeBuffer() const { return nullptr; }

        /// Copies bytes [offset, offset + size) of the GPU buffer into `out`, BLOCKING until
        /// the work already encoded or submitted has finished: the buffer's twin of
        /// Texture::read, for what a compute kernel wrote. `storage()` is the CPU copy the
        /// buffer was created or last written from, never what the GPU wrote since. False
        /// when the range is out of bounds or the backend cannot read back (as Texture::read,
        /// Vulkan refuses while a frame or an offline scope is recording).
        virtual bool read(size_t /*offset*/, size_t /*size*/, void* /*out*/) { return false; }

        /// Count this buffer as a STORAGE buffer in the device's VRAM statistics
        /// rather than a vertex buffer. Called wherever it is bound as storage; the
        /// first call moves its bytes from vb to sb, later ones do nothing.
        void markStorageUse();
        [[nodiscard]] bool storageUse() const { return _storageUse; }

        /** CPU-side vertex data. Used by BatchManager to read vertex positions/normals for merging. */
        const std::vector<uint8_t>& storage() const { return _storage; }

    protected:
        /// Zero-copy constructor: creates a VertexBuffer with no CPU-side _storage.
        /// Used when the GPU buffer is provided externally (e.g., compute shader output).
        /// The _storage vector remains empty — the GPU buffer is set via the subclass.
        VertexBuffer(GraphicsDevice* device, std::shared_ptr<VertexFormat> format,
            int numVertices, int numBytes);

        /// Sends bytes [offset, offset + size) of `_storage` to the GPU. A backend that
        /// can send a part overrides it; the default sends the whole buffer.
        virtual void uploadRange(size_t /*offset*/, size_t /*size*/) { unlock(); }

        GraphicsDevice* _device;

        std::vector<uint8_t> _storage;

    private:
        void adjustVramSizeTracking(DeviceVRAM& vram, int size);

        bool _storageUse = false;

        static int _nextId;

        std::shared_ptr<VertexFormat> _format;
        int _numVertices;
        int _numBytes;
        BufferUsage _usage;
        int _id;
    };
}
