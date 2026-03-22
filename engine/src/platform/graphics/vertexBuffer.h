// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025.
//
#pragma once

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

        // Notifies the graphics engine that the client side copy of the vertex buffer's memory can be
        // returned to the control of the graphics driver.
        virtual void unlock() = 0;

        int numVertices() const { return _numVertices; }

        virtual void* nativeBuffer() const { return nullptr; }

        /** CPU-side vertex data. Used by BatchManager to read vertex positions/normals for merging. */
        const std::vector<uint8_t>& storage() const { return _storage; }

    protected:
        /// Zero-copy constructor: creates a VertexBuffer with no CPU-side _storage.
        /// Used when the GPU buffer is provided externally (e.g., compute shader output).
        /// The _storage vector remains empty — the GPU buffer is set via the subclass.
        VertexBuffer(GraphicsDevice* device, std::shared_ptr<VertexFormat> format,
            int numVertices, int numBytes);

        GraphicsDevice* _device;

        std::vector<uint8_t> _storage;

    private:
        void adjustVramSizeTracking(DeviceVRAM& vram, int size);

        static int _nextId;

        std::shared_ptr<VertexFormat> _format;
        int _numVertices;
        int _numBytes;
        BufferUsage _usage;
        int _id;
    };
}
