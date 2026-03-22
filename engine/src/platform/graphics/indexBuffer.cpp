// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025.
//
#include "indexBuffer.h"

#include <assert.h>

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
    }
}
