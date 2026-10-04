// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025
//
#include "indexBuffer.h"

#include <assert.h>

#include <spdlog/spdlog.h>

#include "graphicsDevice.h"

namespace visutwin::canvas
{
    int IndexBuffer::_nextId = 0;

    IndexBuffer::IndexBuffer(GraphicsDevice* graphicsDevice, IndexFormat format, int numIndices)
        : _device(graphicsDevice), _format(format), _numIndices(numIndices)
    {
        assert(graphicsDevice != nullptr && "GraphicsDevice cannot be null");
        assert(numIndices > 0 && "Number of indices must be greater than 0");
        //assert(IndexBufferUtils::validateParameters(format, numIndices, usage), "Invalid index buffer parameters");

        _numBytes = indexFormatBytes(format) * numIndices;

        // Track VRAM usage, as VertexBuffer does, and register with the device, so its
        // teardown can detach this buffer should it outlive the device.
        if (_device) {
            {
                std::lock_guard lock(_device->_liveResourcesMutex);
                _device->_liveIndexBuffers.insert(this);
            }
            adjustVramSizeTracking(_device->_vram, _numBytes);
        }
    }

    IndexBuffer::~IndexBuffer()
    {
        // Guarded rather than asserted: the constructor's assert is compiled out in
        // a release build, so a null device must not take the destructor with it.
        // A detached buffer has none left to give back.
        if (_device) {
            {
                std::lock_guard lock(_device->_liveResourcesMutex);
                _device->_liveIndexBuffers.erase(this);
            }
            adjustVramSizeTracking(_device->_vram, -_numBytes);
        }
    }

    void IndexBuffer::detachFromDevice()
    {
        // The registry entry is already gone (detachResources took the whole set).
        releaseGpuBuffer();
        adjustVramSizeTracking(_device->_vram, -_numBytes);
        _device = nullptr;
    }

    void IndexBuffer::adjustVramSizeTracking(DeviceVRAM& vram, const int size)
    {
        vram.ib += size;
    }
}
