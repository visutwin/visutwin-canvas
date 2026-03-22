// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// SkinBatchInstance — matrix palette manager for dynamic batching.
//
//
//
// Holds references to the GraphNode of each original mesh instance in a
// dynamic batch.  Each frame, updateMatrices() packs their world transforms
// into a flat float4x4 array (the "palette").  The renderer binds this
// palette at Metal buffer slot 6; the vertex shader indexes into it via
// per-vertex boneIndex to transform vertices on the GPU.
//
// DEVIATION: Standalone class, not extending SkinInstance (which is an
// empty stub).  Uses a Metal buffer instead of upstream's RGBA32F bone
// texture for simpler shader code and idiomatic Metal usage.
//
#pragma once

#include <cstring>
#include <vector>

#include "scene/graphNode.h"

namespace visutwin::canvas
{
    class SkinBatchInstance
    {
    public:
        /// Construct from the GraphNodes of all mesh instances in the batch.
        /// Each node's worldTransform() becomes one "bone" in the palette.
        explicit SkinBatchInstance(std::vector<GraphNode*> nodes);

        /// Pack world matrices from all nodes into the CPU palette.
        /// Called once per frame for each dynamic batch.
        /// 
        void updateMatrices();

        /// CPU-side palette data: N * 16 floats (float4x4 per node).
        [[nodiscard]] const float* paletteData() const { return _palette.data(); }

        /// Total byte size of the palette (for setVertexBytes).
        [[nodiscard]] size_t paletteSizeBytes() const { return _palette.size() * sizeof(float); }

        /// Number of bones (= number of original mesh instances).
        [[nodiscard]] int boneCount() const { return static_cast<int>(_nodes.size()); }

    private:
        std::vector<GraphNode*> _nodes;
        std::vector<float> _palette;  // N x 16 floats (float4x4 per bone)
    };

} // namespace visutwin::canvas
