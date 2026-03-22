// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025.
//
#include "vertexBuffer.h"

#include <assert.h>
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

        // Track VRAM usage
        adjustVramSizeTracking(_device->_vram, _numBytes);

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
        adjustVramSizeTracking(_device->_vram, _numBytes);
    }

    VertexBuffer::~VertexBuffer()
    {
        const auto it = std::find(_device->_buffers.begin(), _device->_buffers.end(), this);
        if (it != _device->_buffers.end())
        {
            _device->_buffers.erase(it);
        }

        // Use _numBytes (not _storage.size()) — correct for both regular and zero-copy paths.
        adjustVramSizeTracking(_device->_vram, -_numBytes);
    }

    void VertexBuffer::adjustVramSizeTracking(DeviceVRAM& vram, int size) {
        spdlog::trace("${this.id} size: ${size} vram.vb: ${vram.vb} => ${vram.vb + size}");
        vram.vb += size;
    }

    bool VertexBuffer::setData(const std::vector<uint8_t>& data)
    {
        if (data.size() != _numBytes) {
            spdlog::error("VertexBuffer: wrong initial data size: expected " +
                std::to_string(_numBytes) + ", got " + std::to_string(data.size()));
            return false;
        }

        _storage = data;
        unlock();
        return true;
    }
}
