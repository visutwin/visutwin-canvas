// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Splitting a bucket of mesh instances into the lists that each become ONE batch
// (upstream's BatchManager.prepare).
//
// A batch is one draw with one vertex buffer, one material and one set of flags,
// so mesh instances that disagree about any of those cannot share it. Merging
// them anyway does not fail — it produces a draw that renders the wrong thing:
// a vertex buffer read through the wrong layout is garbage geometry, and a
// caster merged with a non-caster silently gains or loses its shadow.
//
// Kept free of MeshInstance and of the GPU so the rules are unit-testable
// (tests/batchSplitTests.cpp). BatchManager describes each mesh instance as a
// BatchCandidate and maps the returned indices back.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/shape/boundingBox.h"
#include "platform/graphics/vertexFormat.h"

namespace visutwin::canvas
{
    /**
     * What a batcher needs to know about one mesh instance to decide whether it can
     * share a draw with another. Everything a batch carries exactly once.
     */
    struct BatchCandidate
    {
        /** VertexFormat::batchingHash — the attribute set the merged buffer must have. */
        uint32_t formatBatchingHash = 0;

        /** Primitive type; the merged mesh declares a single one. */
        int primitiveType = 0;

        /** The batch MeshInstance carries one of each of these, not one per source. */
        bool castShadow = false;
        bool receiveShadow = false;

        /** World-space bounds at prepare time, which the maxAabbSize split measures. */
        BoundingBox aabb;
    };

    /** Stride of the packed vertex both merge paths read (position, normal, uv0, tangent, uv1). */
    inline constexpr int kPackedVertexStride = 56;

    /** Upstream's cap on the instances one dynamic batch's matrix palette may hold. */
    inline constexpr size_t kMaxDynamicBatchInstances = 1024;

    /**
     * Splits `candidates` into lists, each of which is valid to merge into one batch.
     * Returns indices into `candidates`, in the order the lists were formed.
     *
     * Mirrors upstream: take the first candidate left, sweep the rest, and push
     * everything incompatible into the leftovers that seed the next list — so
     * compatible instances still meet even when the input interleaves them.
     *
     * `maxAabbSize` is the largest any DIMENSION of a batch's merged bounds may grow
     * to; 0 or less means no limit, which is BatchGroup's default. Splitting on it is
     * what keeps a batch cullable: one batch spanning the whole scene is visible from
     * everywhere and is drawn whatever the camera is looking at.
     *
     * `dynamic` additionally caps a list at kMaxDynamicBatchInstances, since every
     * instance in a dynamic batch owns a matrix in the palette.
     */
    std::vector<std::vector<size_t>> splitBatchLists(const std::vector<BatchCandidate>& candidates,
        float maxAabbSize, bool dynamic);

    /**
     * True when `format` is exactly the 56-byte packed layout the merge paths read —
     * stride, offsets, types and all. Merging reinterprets a source vertex buffer as
     * that struct, so anything else (a skinned mesh's 88 bytes, a point cloud's 28, a
     * custom format) has to be rejected rather than merged: the result would be
     * garbage geometry built from a walk past the end of the source buffer.
     */
    bool formatIsPackedVertexLayout(const VertexFormat& format);
}
