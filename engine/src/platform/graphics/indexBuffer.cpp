// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025.
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

        // Track VRAM usage, as VertexBuffer does. Index memory went uncounted until
        // 2026-09-16, so any total reported before then understated geometry.
        if (_device) {
            adjustVramSizeTracking(_device->_vram, _numBytes);
        }
    }

    IndexBuffer::~IndexBuffer()
    {
        // Guarded rather than asserted: the constructor's assert is compiled out in
        // a release build, so a null device must not take the destructor with it.
        if (_device) {
            adjustVramSizeTracking(_device->_vram, -_numBytes);
        }
    }

    void IndexBuffer::adjustVramSizeTracking(DeviceVRAM& vram, const int size)
    {
        vram.ib += size;
    }
}
