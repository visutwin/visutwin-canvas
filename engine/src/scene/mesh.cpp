// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2025.
//
#include "mesh.h"

namespace visutwin::canvas
{
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