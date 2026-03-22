// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2025.
//
#pragma once

#include <cstddef>

#include <core/shape/boundingBox.h>
#include <core/refCountedObject.h>
#include <platform/graphics/indexBuffer.h>
#include <platform/graphics/vertexBuffer.h>

namespace visutwin::canvas
{
    enum PrimitiveType
    {
        PRIMITIVE_POINTS = 0,
        PRIMITIVE_LINES = 1,
        PRIMITIVE_LINELOOP = 2,
        PRIMITIVE_LINESTRIP = 3,
        PRIMITIVE_TRIANGLES = 4,
        PRIMITIVE_TRISTRIP = 5,
        PRIMITIVE_TRIFAN = 6
    };

    /**
     * Primitive object defining how vertex and index data should be interpreted.
     */
    struct Primitive
    {
        PrimitiveType type = PrimitiveType::PRIMITIVE_TRIANGLES;

        /** Offset of the first index or vertex to dispatch */
        int base = 0;

        /** Number added to each index value before indexing into the vertex buffers (WebGPU only) */
        int baseVertex = 0;

        /** Number of indices or vertices to dispatch */
        int count = 0;

        /** Whether to interpret the primitive as indexed */
        bool indexed = false;
    };

    /**
     * Helper class used to store vertex/index data streams and related properties
     * when mesh is programmatically modified.
     */
    struct GeometryData
    {
        // Maximum number of vertices that can be stored without reallocation
        int maxVertices = 0;

        // Maximum number of indices that can be stored without reallocation
        int maxIndices = 0;

        // Current number of vertices in use
        int vertexCount = 0;

        // Current number of indices in use
        int indexCount = 0;
    };

    /**
     * A graphical primitive. The mesh is defined by a VertexBuffer and an optional
     * IndexBuffer. It also contains a primitive definition which controls the type of the
     * primitive and the portion of the vertex or index buffer to use.
     *
     * Mesh APIs:
     * There are two ways a mesh can be generated or updated.
     *
     * Simple Mesh API:
     * Mesh class provides interfaces such as setPositions() and setUvs()
     * that provide a simple way to provide vertex and index data for the Mesh.
     *
     * Update Vertex and Index buffers:
     * This allows greater flexibility but is more complex to use.
     */
    class Mesh : public RefCountedObject
    {
    public:
        int aabbVer() const { return _aabbVer; }

        const BoundingBox& aabb() const { return _aabb; }

        void setAabb(const BoundingBox& bounds)
        {
            _aabb = bounds;
            _aabbVer++;
        }

        void setVertexBuffer(const std::shared_ptr<VertexBuffer>& vb)
        {
            vertexBuffer = vb;
            _aabbVer++;
        }

        void setIndexBuffer(const std::shared_ptr<IndexBuffer>& ib, const size_t style = 0)
        {
            if (indexBuffer.size() <= style) {
                indexBuffer.resize(style + 1);
            }
            indexBuffer[style] = ib;
            _aabbVer++;
        }

        void setPrimitive(const Primitive& p, const size_t style = 0)
        {
            if (primitive.size() <= style) {
                primitive.resize(style + 1);
            }
            primitive[style] = p;
            _aabbVer++;
        }

        std::shared_ptr<VertexBuffer> getVertexBuffer() const { return vertexBuffer; }

        std::shared_ptr<IndexBuffer> getIndexBuffer(const size_t style = 0) const
        {
            return style < indexBuffer.size() ? indexBuffer[style] : nullptr;
        }

        Primitive getPrimitive(const size_t style = 0) const
        {
            return style < primitive.size() ? primitive[style] : Primitive{};
        }

    private:
        void initGeometryData();

        // Internal AABB version counter
        int _aabbVer = 0;

        // AABB representing object space bounds
        BoundingBox _aabb;

        // Geometry data for programmatic mesh modification
        std::unique_ptr<GeometryData> _geometryData;

        // Array of index buffers for different render styles
        std::vector<std::shared_ptr<IndexBuffer>> indexBuffer;

        // The vertex buffer holding vertex data
        std::shared_ptr<VertexBuffer> vertexBuffer;

        // Array of primitive objects defining how to interpret vertex/index data
        std::vector<Primitive> primitive;
    };
}
