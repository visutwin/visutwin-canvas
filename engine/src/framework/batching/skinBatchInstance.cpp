// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// SkinBatchInstance implementation.
//
//
//
#include "skinBatchInstance.h"

#include "core/math/matrix4.h"

namespace visutwin::canvas
{
    SkinBatchInstance::SkinBatchInstance(std::vector<GraphNode*> nodes)
        : _nodes(std::move(nodes))
    {
        // Pre-allocate palette: 16 floats (float4x4) per bone.
        _palette.resize(_nodes.size() * 16, 0.0f);
    }

    void SkinBatchInstance::updateMatrices()
    {
        // 
        //
        // upstream packs 4x3 matrices (12 floats per bone) to save texture
        // space.  We use float4x4 (16 floats per bone) for simpler Metal
        // shader code — the buffer approach has no texture dimension constraints.
        //
        // DEVIATION: upstream stores transposed rows; we store column-major
        // float4x4 directly from Matrix4, matching Metal's column-major convention.
        //
        // Matrix4 is 64 bytes (4 columns × 4 floats × 4 bytes) on all backends
        // (scalar float[4][4], SSE __m128[4], NEON float32x4_t[4], Apple
        // simd_float4x4) — all column-major, contiguous in memory.  A single
        // memcpy per bone is 8-16× faster than 16 getElement() calls, which on
        // SSE/NEON each do a store-to-temp + scalar extract round-trip.
        static_assert(sizeof(Matrix4) == 64, "Matrix4 must be 64 bytes for palette memcpy");

        const int count = static_cast<int>(_nodes.size());
        for (int i = 0; i < count; ++i)
        {
            const Matrix4& wt = _nodes[i]->worldTransform();
            std::memcpy(&_palette[i * 16], &wt, 64);
        }
    }

} // namespace visutwin::canvas
