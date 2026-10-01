// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2025.
//
#include "mesh.h"

#include <atomic>

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
}