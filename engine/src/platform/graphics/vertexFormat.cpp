// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.09.2025.
//
#include "vertexFormat.h"

#include <functional>

namespace visutwin::canvas
{
    VertexFormat::VertexFormat(const int size, const bool interleaved, const bool instancing)
        : _size(size), _interleaved(interleaved), _instancing(instancing)
    {
        _verticesByteSize = 0;
        _renderingHashString = std::to_string(size) + ":" + (interleaved ? "i" : "ni") + ":" + (instancing ? "1" : "0");
        _renderingHash = static_cast<uint32_t>(std::hash<std::string>{}(_renderingHashString));
    }
}
