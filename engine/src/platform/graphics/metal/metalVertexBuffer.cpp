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
}
