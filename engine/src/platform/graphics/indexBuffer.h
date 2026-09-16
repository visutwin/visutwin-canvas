// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025.
//
#pragma once

#include <cstdint>
#include <vector>

namespace visutwin::canvas
{
    enum IndexFormat {
        INDEXFORMAT_UINT8 = 0,
        INDEXFORMAT_UINT16 = 1,
        INDEXFORMAT_UINT32 = 2
    };

    class GraphicsDevice;
    struct DeviceVRAM;

    /// Bytes one index occupies in the given format. The device's VRAM tracking
    /// and every backend upload size derive from this, so it has exactly one home.
    constexpr int indexFormatBytes(const IndexFormat format)
    {
        switch (format) {
        case INDEXFORMAT_UINT8:  return 1;
        case INDEXFORMAT_UINT16: return 2;
        case INDEXFORMAT_UINT32: return 4;
        }
        return 0;
    }

    /**
     * An index buffer stores index values into a VertexBuffer. Indexed graphical primitives
     * can normally use less memory than unindexed primitives (if vertices are shared).
     *
     * Typically, index buffers are set on Mesh objects.
     */
    class IndexBuffer
    {
    public:
        IndexBuffer(GraphicsDevice* graphicsDevice, IndexFormat format, int numIndices);

        // Defined rather than defaulted: the destructor releases this buffer's
        // share of the device's tracked VRAM, as VertexBuffer's does.
        virtual ~IndexBuffer();

        IndexFormat format() const { return _format; }

        int numIndices() const { return _numIndices; }

        /// Size of the index data in bytes — what the device's VRAM tracking counts.
        int numBytes() const { return _numBytes; }

        virtual void* nativeBuffer() const { return nullptr; }

        virtual bool setData(const std::vector<uint8_t>& data) { (void)data; return false; }

        /** CPU-side index data. Used by BatchManager to read indices for merging. */
        const std::vector<uint8_t>& storage() const { return _storage; }

    private:
        static int _nextId;

        static void adjustVramSizeTracking(DeviceVRAM& vram, int size);

    protected:
        GraphicsDevice* _device = nullptr;
        std::vector<uint8_t> _storage;

    private:
        IndexFormat _format;

        int _numIndices;

        int _numBytes = 0;
    };
}
