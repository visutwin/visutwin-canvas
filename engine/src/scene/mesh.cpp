// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2025
//
#include "mesh.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    uint32_t Mesh::nextId()
    {
        // Atomic: a mesh may be built off the main thread.
        static std::atomic<uint32_t> next{0};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    void Mesh::initGeometryData()
    {
        if (!_geometryData) {
            _geometryData = std::make_unique<GeometryData>();

            // Store existing sizes if buffers exist
            if (_vertexBuffer) {
                _geometryData->vertexCount = _vertexBuffer->numVertices();
                _geometryData->maxVertices = _vertexBuffer->numVertices();
            }

            if (!_indexBuffer.empty() && _indexBuffer[0]) {
                _geometryData->indexCount = _indexBuffer[0]->numIndices();
                _geometryData->maxIndices = _indexBuffer[0]->numIndices();
            }
        }
    }

    namespace
    {
        uint32_t readIndex(const std::vector<uint8_t>& storage, const IndexFormat format, const size_t i)
        {
            switch (format) {
            case INDEXFORMAT_UINT8: return storage[i];
            case INDEXFORMAT_UINT16: {
                uint16_t v;
                std::memcpy(&v, storage.data() + i * 2, 2);
                return v;
            }
            case INDEXFORMAT_UINT32: {
                uint32_t v;
                std::memcpy(&v, storage.data() + i * 4, 4);
                return v;
            }
            }
            return 0;
        }

        void appendIndex(std::vector<uint8_t>& bytes, const IndexFormat format, const uint32_t value)
        {
            const int size = indexFormatBytes(format);
            const size_t at = bytes.size();
            bytes.resize(at + static_cast<size_t>(size));
            if (format == INDEXFORMAT_UINT8) {
                bytes[at] = static_cast<uint8_t>(value);
            } else if (format == INDEXFORMAT_UINT16) {
                const auto v = static_cast<uint16_t>(value);
                std::memcpy(bytes.data() + at, &v, 2);
            } else {
                std::memcpy(bytes.data() + at, &value, 4);
            }
        }
    }

    void Mesh::generateWireframe()
    {
        GraphicsDevice* device = _vertexBuffer ? _vertexBuffer->device() : nullptr;
        if (!device) {
            return;
        }
        if (_indexBuffer.size() > 1) {
            _indexBuffer[1].reset();
        }
        const auto numVertices = static_cast<uint64_t>(_vertexBuffer->numVertices());
        const Primitive solid = getPrimitive(0);

        std::vector<uint8_t> lines;
        IndexFormat format;
        if (const auto& source = getIndexBuffer(0)) {
            // Each edge once, whichever triangle meets it first and in its winding.
            format = source->format();
            const auto& storage = source->storage();
            const size_t base = static_cast<size_t>(solid.base);
            const size_t end = std::min(base + static_cast<size_t>(solid.count),
                storage.size() / static_cast<size_t>(indexFormatBytes(format)));
            const auto baseVertex = static_cast<uint32_t>(solid.baseVertex);
            std::unordered_set<uint64_t> seen;
            constexpr int kEdges[3][2] = {{0, 1}, {1, 2}, {2, 0}};
            for (size_t j = base; j + 2 < end; j += 3) {
                for (const auto& edge : kEdges) {
                    const uint32_t i1 = readIndex(storage, format, j + edge[0]) + baseVertex;
                    const uint32_t i2 = readIndex(storage, format, j + edge[1]) + baseVertex;
                    const uint64_t key = i1 > i2 ? i2 * numVertices + i1 : i1 * numVertices + i2;
                    if (seen.insert(key).second) {
                        appendIndex(lines, format, i1);
                        appendIndex(lines, format, i2);
                    }
                }
            }
        } else {
            const uint64_t triangles = numVertices / 3;
            format = triangles * 6 > 65535 ? INDEXFORMAT_UINT32 : INDEXFORMAT_UINT16;
            for (uint64_t t = 0; t < triangles; ++t) {
                const auto v = static_cast<uint32_t>(t * 3);
                for (const uint32_t index : {v, v + 1, v + 1, v + 2, v + 2, v}) {
                    appendIndex(lines, format, index);
                }
            }
        }

        const int count = static_cast<int>(lines.size() / static_cast<size_t>(indexFormatBytes(format)));
        Primitive wireframe;
        wireframe.type = PRIMITIVE_LINES;
        wireframe.count = count;
        wireframe.indexed = true;
        setIndexBuffer(count > 0 ? device->createIndexBuffer(format, count, lines) : nullptr, 1);
        setPrimitive(count > 0 ? wireframe : Primitive{}, 1);
    }
}
