// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The key the forward pass orders opaque draws on (upstream MeshInstance.updateKey).
//
// Kept as a free function over plain inputs so a unit test can hold the bit layout
// without a renderer or a material — a field that silently overlaps its neighbour is
// invisible in any render, which is exactly what happened to the key this replaced.
#pragma once

#include <cstdint>

namespace visutwin::canvas
{
    /**
     * Packs the ordering fields into one comparable integer, most significant first:
     *
     *   63..56  draw bucket   — a block drawn entirely before the next, whatever the
     *                           materials in it are
     *   55      alpha test    — masked materials after plain opaque ones, so they do
     *                           not disable the early depth test for everything behind
     *   54..32  material ID   — identity, so consecutive draws of ONE material skip
     *                           binding state altogether
     *   31..0   mesh address  — meshes of one material stay together, which is one
     *                           vertex-buffer bind instead of many
     *
     * Every field owns its own bits. The key this replaced XORed overlapping ranges:
     * the depth-state key and the emissive-texture bit both landed on bit 4, the alpha
     * mode and the occlusion bit both on bit 3. Two materials differing in one of
     * those could produce the same key and interleave with each other's draws. It then
     * shifted the whole result left by 32, discarding the half that held the shader
     * variant key — so the most expensive state change in a frame contributed nothing
     * to the order at all.
     *
     * `materialId` is masked to 23 bits; a process that somehow creates more than
     * eight million materials wraps and sorts two of them together, which costs a
     * state change and nothing else.
     *
     * DEVIATION: upstream's key is 32 bits and it compares mesh id separately as a
     * tiebreak. Widening to 64 folds the mesh in, so the comparator is one integer
     * compare and no caller has to remember the second step.
     */
    inline uint64_t makeForwardSortKey(const uint8_t drawBucket, const bool alphaTest,
        const uint32_t materialId, const uintptr_t meshAddress)
    {
        const uint64_t bucket = drawBucket;
        const uint64_t masked = alphaTest ? 1u : 0u;
        const uint64_t material = materialId & 0x7FFFFFu;
        // The low bits of a pointer are alignment padding and carry no information.
        const uint64_t mesh = static_cast<uint64_t>(meshAddress >> 4) & 0xFFFFFFFFu;
        return (bucket << 56) | (masked << 55) | (material << 32) | mesh;
    }
}
