// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "morph.h"

#include <cstring>

#include <spdlog/spdlog.h>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"

namespace visutwin::canvas
{
    Morph::Morph(std::vector<MorphTarget> targets, const int vertexCount, GraphicsDevice* device)
        : _targets(std::move(targets)), _vertexCount(vertexCount)
    {
        if (_targets.empty() || _vertexCount <= 0 || !device) {
            return;
        }

        // Pack all targets into one buffer: per target, per vertex, a float4 position
        // delta followed by a float4 normal delta (w unused, kept for alignment).
        constexpr int floatsPerVertex = 8;
        const size_t totalFloats = _targets.size() * static_cast<size_t>(_vertexCount) * floatsPerVertex;
        std::vector<uint8_t> bytes(totalFloats * sizeof(float), 0);
        auto* dst = reinterpret_cast<float*>(bytes.data());

        for (size_t t = 0; t < _targets.size(); ++t) {
            const auto& target = _targets[t];
            const bool hasNormals = target.deltaNormals.size() >= static_cast<size_t>(_vertexCount) * 3;
            const bool hasPositions = target.deltaPositions.size() >= static_cast<size_t>(_vertexCount) * 3;
            if (!hasPositions) {
                spdlog::warn("Morph target '{}' has {} position floats, expected {} — zero-filled",
                    target.name, target.deltaPositions.size(), static_cast<size_t>(_vertexCount) * 3);
            }
            for (int i = 0; i < _vertexCount; ++i) {
                float* v = dst + (t * static_cast<size_t>(_vertexCount) + i) * floatsPerVertex;
                if (hasPositions) {
                    v[0] = target.deltaPositions[i * 3 + 0];
                    v[1] = target.deltaPositions[i * 3 + 1];
                    v[2] = target.deltaPositions[i * 3 + 2];
                }
                if (hasNormals) {
                    v[4] = target.deltaNormals[i * 3 + 0];
                    v[5] = target.deltaNormals[i * 3 + 1];
                    v[6] = target.deltaNormals[i * 3 + 2];
                }
            }
        }

        // Storage buffer for the vertex shader (not part of the vertex descriptor);
        // element size is one float4 so the shader can index it as `constant float4*`.
        auto format = std::make_shared<VertexFormat>(4 * static_cast<int>(sizeof(float)), true, false);
        VertexBufferOptions options;
        options.data = std::move(bytes);
        _deltaBuffer = device->createVertexBuffer(format,
            static_cast<int>(_targets.size()) * _vertexCount * 2, options);
        if (!_deltaBuffer) {
            spdlog::error("Morph: delta buffer creation failed ({} targets, {} vertices)",
                _targets.size(), _vertexCount);
        }
    }
}
