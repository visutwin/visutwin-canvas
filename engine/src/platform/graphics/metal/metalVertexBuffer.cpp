// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis  on 25.10.2025.
//
#include "metalVertexBuffer.h"

namespace visutwin::canvas
{
    MetalVertexBuffer::MetalVertexBuffer(GraphicsDevice* graphicsDevice, const std::shared_ptr<VertexFormat>& format,
        int numVertices, const VertexBufferOptions& options)
        : VertexBuffer(graphicsDevice, format, numVertices, options), MetalBuffer(gpu::BufferUsage::VERTEX)
    {
        if (!_storage.empty()) {
            unlock();
        }
    }

    MetalVertexBuffer::MetalVertexBuffer(GraphicsDevice* device, const std::shared_ptr<VertexFormat>& format,
        int numVertices, MTL::Buffer* externalBuffer)
        : VertexBuffer(device, format, numVertices,
                       static_cast<int>(format->size()) * numVertices),
          MetalBuffer(gpu::BufferUsage::VERTEX)
    {
        adoptBuffer(externalBuffer);  // Takes ownership via retain
    }

    void MetalVertexBuffer::unlock()
    {
        MetalBuffer::unlock(static_cast<MetalGraphicsDevice*>(_device), _storage);
    }

    void MetalVertexBuffer::uploadRange(const size_t offset, const size_t size)
    {
        if (!raw() || offset + size > MetalBuffer::size()) {
            unlock();   // not allocated yet: the whole buffer, which allocates it
            return;
        }
        // Shared storage: the bytes are the GPU's as soon as they are copied.
        write(offset, _storage.data() + offset, size);
    }
}
