// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.07.2025.
//
#include "metalBuffer.h"

#include <spdlog/spdlog.h>

namespace visutwin::canvas::gpu
{
    MetalBuffer::~MetalBuffer()
    {
        if (_buffer)
        {
            _buffer->release();
        }
    }

    void MetalBuffer::allocate(MTL::Device* device, size_t size)
    {
        assert(!_buffer && "Buffer already allocated");

        auto options = MTL::StorageModeShared;
        _buffer = device->newBuffer(size, options);

        auto label = "";
        if (_usageFlags & VERTEX) {
            label = "VertexBuffer";
        } else if (_usageFlags & INDEX) {
            label = "IndexBuffer";
        } else if (_usageFlags & UNIFORM) {
            label = "UniformBuffer";
        } else if (_usageFlags & STORAGE) {
            label = "StorageBuffer";
        }
        _buffer->addDebugMarker(NS::String::string(label, NS::UTF8StringEncoding), NS::Range(0, size - 1));
    }

    void MetalBuffer::write(size_t bufferOffset, const void* data, size_t dataSize) const
    {
        assert(bufferOffset + dataSize <= size());

        memcpy(static_cast<uint8_t*>(_buffer->contents()) + bufferOffset, data, dataSize);
    }

    void MetalBuffer::unlock(MetalGraphicsDevice* device, const std::vector<uint8_t>& storage)
    {
        assert(device != nullptr && "Cannot unlock buffer without valid device");

        if (!_buffer) {
            // Size needs to be a multiple of 4 for WebGPU
            size_t size = (storage.size() + 3) & ~3;

            // Add COPY_DST usage flag for data uploads
            _usageFlags = static_cast<BufferUsage>(_usageFlags | BufferUsage::COPY_DST);

            allocate(device, size);
        }

        if (storage.empty()) {
            return;
        }

        // Prepare data for upload - WebGPU requires proper alignment
        size_t totalSize = _buffer->length();

        write(0, storage.data(), totalSize);

        spdlog::debug("WriteBuffer: {} (size: {} bytes)", _buffer->label()->utf8String(), totalSize);
    }

    void MetalBuffer::allocate(MetalGraphicsDevice* device, size_t size)
    {
        assert(!_buffer && "Buffer already allocated");

        if (!device) {
            spdlog::error("Cannot allocate buffer without valid device");
            return;
        }

        MTL::ResourceOptions options = MTL::ResourceStorageModeShared;
        // Cache mode adjustments for uniform buffers
        if (_usageFlags & UNIFORM) {
            options |= MTL::ResourceCPUCacheModeDefaultCache;
        } else {
            options |= MTL::ResourceCPUCacheModeWriteCombined;
        }

        _buffer = device->raw()->newBuffer(size, options);

        // Set the debug label based on usage
        const char* label = "";
        if (_usageFlags & VERTEX) {
            label = "VertexBuffer";
        } else if (_usageFlags & INDEX) {
            label = "IndexBuffer";
        } else if (_usageFlags & UNIFORM) {
            label = "UniformBuffer";
        } else if (_usageFlags & STORAGE) {
            label = "StorageBuffer";
        }
        _buffer->setLabel(NS::String::string(label, NS::UTF8StringEncoding));

        spdlog::debug("Allocated Metal buffer: {} (size: {} bytes)", label, size);
    }

    void MetalBuffer::upload(GraphicsDevice* device, const void* data, size_t dataSize)
    {
        auto* metalDevice = static_cast<MetalGraphicsDevice*>(device);
        if (!_buffer) {
            allocate(metalDevice, dataSize);
        }
        if (data && dataSize > 0) {
            write(0, data, dataSize);
        }
    }

    void MetalBuffer::adoptBuffer(MTL::Buffer* buffer)
    {
        if (_buffer) {
            _buffer->release();
        }
        _buffer = buffer;
        if (_buffer) {
            _buffer->retain();  // We now own a reference
        }
    }
}
