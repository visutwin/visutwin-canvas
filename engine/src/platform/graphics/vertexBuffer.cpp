// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025
//
#include "vertexBuffer.h"

#include <assert.h>
#include <cstring>
#include <spdlog/spdlog.h>

#include "graphicsDevice.h"

namespace visutwin::canvas
{
    int VertexBuffer::_nextId = 0;

    VertexBuffer::VertexBuffer(GraphicsDevice* graphicsDevice, std::shared_ptr<VertexFormat> format,
        int numVertices, const VertexBufferOptions& options)
    : _device(graphicsDevice), _format(format), _numVertices(numVertices), _usage(options.usage), _id(_nextId++) {

        assert(graphicsDevice != nullptr && "GraphicsDevice cannot be null");
        assert(format != nullptr && "VertexFormat cannot be null");
        assert(numVertices > 0 && "Number of vertices must be greater than 0");

        // Calculate the size. If format contains verticesByteSize (non-interleaved format), use it
        _numBytes = format->verticesByteSize() ? format->verticesByteSize() : format->size() * numVertices;

        attachToDevice();

        // Allocate the storage
        if (!options.data.empty()) {
            if (options.data.size() != static_cast<size_t>(_numBytes)) {
                spdlog::error("VertexBuffer: wrong initial data size: expected {}, got {}", _numBytes, options.data.size());
                _storage.resize(_numBytes, 0);
            } else {
                _storage = options.data;
            }
        } else {
            _storage.resize(_numBytes, 0); // Initialize with zeros
        }
    }

    VertexBuffer::VertexBuffer(GraphicsDevice* device, std::shared_ptr<VertexFormat> format,
        int numVertices, int numBytes)
    : _device(device), _format(std::move(format)), _numVertices(numVertices),
      _numBytes(numBytes), _usage(BUFFER_STATIC), _id(_nextId++) {
        // Zero-copy: _storage intentionally left empty — GPU buffer provided externally.
        attachToDevice();
    }

    VertexBuffer::~VertexBuffer()
    {
        // A buffer the device already detached has nothing left to give back.
        if (!_device) {
            return;
        }
        {
            std::lock_guard lock(_device->_liveResourcesMutex);
            _device->_liveVertexBuffers.erase(this);
        }
        // Use _numBytes (not _storage.size()) — correct for both regular and zero-copy paths.
        adjustVramSizeTracking(_device->_vram, -_numBytes);
    }

    void VertexBuffer::attachToDevice()
    {
        // Registered with the device, so its teardown can detach this buffer should it
        // outlive the device.
        {
            std::lock_guard lock(_device->_liveResourcesMutex);
            _device->_liveVertexBuffers.insert(this);
        }
        adjustVramSizeTracking(_device->_vram, _numBytes);
    }

    void VertexBuffer::detachFromDevice()
    {
        // The registry entry is already gone (detachResources took the whole set).
        releaseGpuBuffer();
        adjustVramSizeTracking(_device->_vram, -_numBytes);
        _device = nullptr;
    }

    void VertexBuffer::adjustVramSizeTracking(DeviceVRAM& vram, int size) {
        if (_storageUse) {
            vram.sb += size;
            _device->_storageVertexBufferBytes += size;
        } else {
            vram.vb += size;
        }
    }

    void VertexBuffer::markStorageUse()
    {
        if (_storageUse || !_device) {
            return;
        }
        adjustVramSizeTracking(_device->_vram, -_numBytes);
        _storageUse = true;
        adjustVramSizeTracking(_device->_vram, _numBytes);
    }

    bool VertexBuffer::writeRange(const size_t offset, const void* data, const size_t size)
    {
        if (!_device || !data || size == 0 || offset + size > _storage.size()) {
            return false;
        }
        std::memcpy(_storage.data() + offset, data, size);
        uploadRange(offset, size);
        return true;
    }

    bool VertexBuffer::setData(const std::vector<uint8_t>& data)
    {
        if (data.size() != static_cast<size_t>(_numBytes)) {
            spdlog::error("VertexBuffer: wrong initial data size: expected " +
                std::to_string(_numBytes) + ", got " + std::to_string(data.size()));
            return false;
        }

        _storage = data;
        if (_device) {
            unlock();
        }
        return true;
    }
}
