// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2025.
//
#include "metalIndexBuffer.h"

#include "metalGraphicsDevice.h"

namespace visutwin::canvas
{
    namespace
    {
        size_t indexElementSize(const IndexFormat format)
        {
            switch (format) {
            case INDEXFORMAT_UINT8:
                return 1;
            case INDEXFORMAT_UINT16:
                return 2;
            case INDEXFORMAT_UINT32:
                return 4;
            default:
                return 2;
            }
        }
    }

    MetalIndexBuffer::MetalIndexBuffer(GraphicsDevice* graphicsDevice, const IndexFormat format, const int numIndices)
        : IndexBuffer(graphicsDevice, format, numIndices), MetalBuffer(gpu::BufferUsage::INDEX)
    {
    }

    bool MetalIndexBuffer::setData(const std::vector<uint8_t>& data)
    {
        const auto expectedSize = static_cast<size_t>(numIndices()) * indexElementSize(format());
        if (data.size() != expectedSize) {
            return false;
        }

        _storage = data;
        MetalBuffer::unlock(static_cast<MetalGraphicsDevice*>(_device), _storage);
        return true;
    }
} 
