// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.10.2025
//
#include "metalVertexBuffer.h"

#include <cstring>

#include "metalGraphicsDevice.h"

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

    bool MetalVertexBuffer::read(const size_t offset, const size_t size, void* out)
    {
        MTL::Buffer* buffer = raw();
        if (!buffer || !out || offset + size > buffer->length()) {
            return false;
        }
        // Shared storage, so the bytes are readable in place once the GPU is done with
        // them. Whatever the frame has encoded is committed first (it is in the open
        // command buffer otherwise, behind the read), and an empty buffer committed after
        // it on the same queue completes only once everything before it has.
        auto* device = static_cast<MetalGraphicsDevice*>(_device);
        device->flushCommands();
        if (MTL::CommandBuffer* fence = device->commandQueue()->commandBuffer()) {
            fence->commit();
            fence->waitUntilCompleted();
        }
        std::memcpy(out, static_cast<const uint8_t*>(buffer->contents()) + offset, size);
        return true;
    }
}
