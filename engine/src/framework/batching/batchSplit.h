// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2026
//
// Splitting a bucket of mesh instances into the lists that each become ONE batch.
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

        /**
         * MeshInstance::mask(): which lights and cameras reach the mesh. One batch draws
         * with one mask, so a source lit by a different light set cannot share it.
         */
        uint32_t mask = 0;

        /**
         * The stencil test and write the mesh draws with (a UI mask level). A batch
         * carries one; `hasStencil` false means the stencil is off and the keys are 0.
         */
        bool hasStencil = false;
        uint32_t stencilFrontKey = 0;
        uint32_t stencilBackKey = 0;

        /** MeshInstance::drawBucket(): the primary sort key, so one bucket per batch. */
        uint8_t drawBucket = 127;

        /** MeshInstance::drawOrder(), which orders a translucent bucket (see below). */
        double drawOrder = 0.0;
    };

    /** Stride of the packed vertex both merge paths read (position, normal, uv0, tangent, uv1). */
    inline constexpr int kPackedVertexStride = 56;

    /** Cap on the instances one dynamic batch's matrix palette may hold. */
    inline constexpr size_t kMaxDynamicBatchInstances = 1024;

    /**
     * Splits `candidates` into lists, each of which is valid to merge into one batch.
     * Returns indices into `candidates`, in the order the lists were formed.
     *
     * Take the first candidate left, sweep the rest, and push
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
     *
     * `translucent` is for a bucket whose material blends, where the order of the
     * draws is part of the picture: the candidates are taken in drawOrder, and one is
     * not merged past a skipped candidate it overlaps unless the two share a draw
     * order — merging it would draw it in the first candidate's place, ahead of or
     * behind a surface it has to composite over.
     *
     * A source's mirroring is not a split key: the merge reverses a mirrored source's
     * triangle winding instead (see BatchManager), so it shares the batch.
     */
    std::vector<std::vector<size_t>> splitBatchLists(const std::vector<BatchCandidate>& candidates,
        float maxAabbSize, bool dynamic, bool translucent = false);

    /**
     * Appends one source's triangle-list indices to a merged list, offset by
     * `vertexOffset`. `indices` holds `indexCount` indices of `indexSize` bytes (1, 2
     * or 4); a null `indices` means the source is not indexed and every `vertexCount`
     * vertices in order are its triangles. `reverseWinding` swaps the last two corners
     * of every triangle — what a source with a mirrored world transform needs once its
     * transform no longer travels with the draw.
     */
    void appendTriangleIndices(std::vector<uint32_t>& merged, const uint8_t* indices, int indexSize,
        int indexCount, int vertexCount, uint32_t vertexOffset, bool reverseWinding);

    /**
     * Whether a render component's mesh instances may be batched at all, given
     * whether each is skinned or morphed.
     * If ANY instance on the entity deforms, the WHOLE entity is excluded.
     *
     * Merging bakes each source's world transform into the shared vertex buffer, so
     * a deforming mesh loses exactly the thing that makes it deform. A skinned one
     * is caught anyway by the layout check below — its 88-byte stride is not the
     * packed one — but a MORPHED mesh carries the ordinary 56-byte layout and its
     * deltas in a separate buffer, so nothing about its format says it must not be
     * merged. It would batch cleanly and then sit still.
     *
     * Whole-entity rather than per-instance:
     * an entity's instances are authored as one thing, and batching half of it leaves
     * the deforming half drawn separately with no indication why.
     */
    bool entityIsBatchable(const std::vector<bool>& instanceDeforms);

    /**
     * True when `format` is exactly the 56-byte packed layout the merge paths read —
     * stride, offsets, types and all. Merging reinterprets a source vertex buffer as
     * that struct, so anything else (a skinned mesh's 88 bytes, a point cloud's 28, a
     * custom format) has to be rejected rather than merged: the result would be
     * garbage geometry built from a walk past the end of the source buffer.
     */
    bool formatIsPackedVertexLayout(const VertexFormat& format);
}
