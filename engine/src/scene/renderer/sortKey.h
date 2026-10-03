// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.09.2026
//
// The key the forward pass orders opaque draws on.
//
// Kept as a free function over plain inputs so a unit test can hold the bit layout
// without a renderer or a material — a field that silently overlaps its neighbour is
// invisible in any render.
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
     *   31..0   mesh ID       — meshes of one material stay together, which is one
     *                           vertex-buffer bind instead of many
     *
     * The mesh field is `Mesh::id()`, a creation-order counter, and never the mesh's
     * ADDRESS. The order of one material's draws decides which of two coplanar
     * surfaces is drawn last and so wins the LESS_EQUAL depth test; ordered by address
     * that follows wherever the heap put the meshes, and the same scene renders
     * different pixels from one run to the next.
     *
     * Every field owns its own bits; never XOR fields into overlapping ranges, or two
     * materials differing in one of them can produce the same key and interleave with
     * each other's draws.
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
        const uint32_t materialId, const uint32_t meshId)
    {
        const uint64_t bucket = drawBucket;
        const uint64_t masked = alphaTest ? 1u : 0u;
        const uint64_t material = materialId & 0x7FFFFFu;
        const uint64_t mesh = meshId;
        return (bucket << 56) | (masked << 55) | (material << 32) | mesh;
    }

    /** The draw bucket a forward sort key was packed with (its top 8 bits). */
    inline uint8_t forwardSortKeyBucket(const uint64_t sortKey)
    {
        return static_cast<uint8_t>(sortKey >> 56);
    }

    /**
     * The order of the two distance sort modes. The draw bucket is the PRIMARY key in
     * both, as it is for the material sort: back to front draws the HIGHER bucket
     * first, front to back the LOWER one (the direction follows the mode, so a bucket
     * that must come last in a back-to-front sublayer is a low one). Within a bucket
     * the signed view depth decides (sortDistance.h), and an exact tie falls back to
     * the forward key, so equal depths keep a stable material grouping.
     *
     * Returns true when the draw (keyA, distanceA) goes before (keyB, distanceB).
     */
    inline bool distanceSortsBefore(const uint64_t keyA, const float distanceA,
        const uint64_t keyB, const float distanceB, const bool backToFront)
    {
        const uint8_t bucketA = forwardSortKeyBucket(keyA);
        const uint8_t bucketB = forwardSortKeyBucket(keyB);
        if (bucketA != bucketB) {
            return backToFront ? bucketA > bucketB : bucketA < bucketB;
        }
        if (distanceA != distanceB) {
            return backToFront ? distanceA > distanceB : distanceA < distanceB;
        }
        return keyA < keyB;
    }
}
