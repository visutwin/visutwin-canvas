// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025.
//
#include "vertexFormat.h"

#include <functional>
#include <sstream>
#include <utility>

namespace visutwin::canvas
{
    VertexFormat::VertexFormat(const int size, const bool interleaved, const bool instancing)
        : VertexFormat(size, {}, interleaved, instancing)
    {
    }

    VertexFormat::VertexFormat(const int size, std::vector<VertexElement> elements,
        const bool interleaved, const bool instancing)
        : _size(size), _interleaved(interleaved), _instancing(instancing),
          _elements(std::move(elements))
    {
        _verticesByteSize = 0;
        std::ostringstream hash;
        hash << size << ':' << (interleaved ? "i" : "ni") << ':' << (instancing ? '1' : '0');
        for (const auto& element : _elements) {
            hash << ':' << static_cast<int>(element.semantic)
                 << ',' << static_cast<int>(element.dataType)
                 << ',' << static_cast<int>(element.componentCount)
                 << ',' << element.offset
                 << ',' << (element.normalized ? '1' : '0');
        }
        _renderingHashString = hash.str();
        _renderingHash = static_cast<uint32_t>(std::hash<std::string>{}(_renderingHashString));
    }

    std::vector<VertexElement> VertexFormat::standardElements()
    {
        return {
            {VertexSemantic::SEMANTIC_POSITION, VertexDataType::TYPE_FLOAT32, 3, 0},
            {VertexSemantic::SEMANTIC_NORMAL, VertexDataType::TYPE_FLOAT32, 3, 12},
            {VertexSemantic::SEMANTIC_TEXCOORD0, VertexDataType::TYPE_FLOAT32, 2, 24},
            {VertexSemantic::SEMANTIC_TANGENT, VertexDataType::TYPE_FLOAT32, 4, 32},
            {VertexSemantic::SEMANTIC_TEXCOORD1, VertexDataType::TYPE_FLOAT32, 2, 48},
        };
    }

    std::vector<VertexElement> VertexFormat::pointElements()
    {
        return {
            {VertexSemantic::SEMANTIC_POSITION, VertexDataType::TYPE_FLOAT32, 3, 0},
            {VertexSemantic::SEMANTIC_COLOR, VertexDataType::TYPE_FLOAT32, 4, 12},
        };
    }

    std::vector<VertexElement> VertexFormat::skinnedElements()
    {
        auto elements = standardElements();
        elements.push_back(
            {VertexSemantic::SEMANTIC_BLENDWEIGHT, VertexDataType::TYPE_FLOAT32, 4, 56});
        elements.push_back(
            {VertexSemantic::SEMANTIC_BLENDINDICES, VertexDataType::TYPE_FLOAT32, 4, 72});
        return elements;
    }

    std::vector<VertexElement> VertexFormat::instanceMatrixElements()
    {
        return {
            {VertexSemantic::SEMANTIC_ATTR5, VertexDataType::TYPE_FLOAT32, 4, 0},
            {VertexSemantic::SEMANTIC_ATTR6, VertexDataType::TYPE_FLOAT32, 4, 16},
            {VertexSemantic::SEMANTIC_ATTR7, VertexDataType::TYPE_FLOAT32, 4, 32},
            {VertexSemantic::SEMANTIC_ATTR8, VertexDataType::TYPE_FLOAT32, 4, 48},
        };
    }
}
