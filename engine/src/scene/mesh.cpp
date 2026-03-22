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
            if (vertexBuffer) {
                _geometryData->vertexCount = vertexBuffer->numVertices();
                _geometryData->maxVertices = vertexBuffer->numVertices();
            }

            if (!indexBuffer.empty() && indexBuffer[0]) {
                _geometryData->indexCount = indexBuffer[0]->numIndices();
                _geometryData->maxIndices = indexBuffer[0]->numIndices();
            }
        }
    }
}