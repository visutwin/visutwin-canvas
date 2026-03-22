// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025.
//
#pragma once

#include <cstdint>
#include <string>

namespace visutwin::canvas
{
    // Vertex attribute semantic constants
    enum class VertexSemantic {
        SEMANTIC_POSITION,
        SEMANTIC_NORMAL,
        SEMANTIC_TANGENT,
        SEMANTIC_BLENDWEIGHT,
        SEMANTIC_BLENDINDICES,
        SEMANTIC_COLOR,
        SEMANTIC_TEXCOORD,
        SEMANTIC_TEXCOORD0,
        SEMANTIC_TEXCOORD1,
        SEMANTIC_TEXCOORD2,
        SEMANTIC_TEXCOORD3,
        SEMANTIC_TEXCOORD4,
        SEMANTIC_TEXCOORD5,
        SEMANTIC_TEXCOORD6,
        SEMANTIC_TEXCOORD7,
        SEMANTIC_ATTR0,
        SEMANTIC_ATTR1,
        SEMANTIC_ATTR2,
        SEMANTIC_ATTR3,
        SEMANTIC_ATTR4,
        SEMANTIC_ATTR5,
        SEMANTIC_ATTR6,
        SEMANTIC_ATTR7,
        SEMANTIC_ATTR8,
        SEMANTIC_ATTR9,
        SEMANTIC_ATTR10,
        SEMANTIC_ATTR11,
        SEMANTIC_ATTR12,
        SEMANTIC_ATTR13,
        SEMANTIC_ATTR14,
        SEMANTIC_ATTR15
    };

    // Vertex data type constants
    enum class VertexDataType {
        TYPE_INT8 = 0,
        TYPE_UINT8 = 1,
        TYPE_INT16 = 2,
        TYPE_UINT16 = 3,
        TYPE_INT32 = 4,
        TYPE_UINT32 = 5,
        TYPE_FLOAT32 = 6,
        TYPE_FLOAT16 = 7
    };

    /**
     * A vertex format is a descriptor that defines the layout of vertex data inside a VertexBuffer
     */
    class VertexFormat
    {
    public:
        VertexFormat(int size, bool interleaved = true, bool instancing = false);

        int size() const { return _size; }

        int verticesByteSize() const { return _verticesByteSize; }

        // Get the rendering hash for fast comparison
        uint32_t renderingHash() const { return _renderingHash; }

        const std::string& renderingHashString() const { return _renderingHashString; }

        bool isInterleaved() const { return _interleaved; }

        bool isInstancing() const { return _instancing; }

    private:
        int _verticesByteSize = 0;

        int _size;

        uint32_t _renderingHash;

        std::string _renderingHashString;

        bool _interleaved;
        bool _instancing;
    };
}
