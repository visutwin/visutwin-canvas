// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2026
//
// Batching merges many mesh instances into one draw, which means one vertex
// layout, one primitive type, one pair of shadow flags and one bounding box. A
// batcher that applies none of those rules reinterprets any vertex buffer as the
// 56-byte packed layout and ignores BatchGroup::maxAabbSize, so a mesh with a
// different stride merges as garbage and one batch can span the whole scene.
// These hold the splitting rules that prevent that.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/shape/boundingBox.h"
#include "framework/batching/batchSplit.h"
#include "platform/graphics/vertexFormat.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // A unit cube centred at x, so a run of them along X grows the batch's bounds
    // by exactly one unit each.
    BatchCandidate cubeAt(const float x, const uint32_t formatHash = 1u)
    {
        BatchCandidate candidate;
        candidate.formatBatchingHash = formatHash;
        candidate.aabb = BoundingBox(Vector3(x, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f));
        return candidate;
    }

    size_t listsCovering(const std::vector<std::vector<size_t>>& lists)
    {
        size_t total = 0;
        for (const auto& list : lists) {
            total += list.size();
        }
        return total;
    }
}

namespace
{
    bool checkEntityBatchable()
    {
        // The WHOLE entity is excluded when any of its mesh instances deforms.
        check(entityIsBatchable({}), "an entity with no mesh instances is batchable");
        check(entityIsBatchable({false, false, false}),
            "an entity whose instances all hold still is batchable");
        check(!entityIsBatchable({false, true, false}),
            "ONE deforming instance excludes the whole entity, not just itself");
        check(!entityIsBatchable({true}), "a single deforming instance excludes it");
        return true;
    }
}

int main()
{
    checkEntityBatchable();

    std::cout << "Batch splitting\n";

    // Nothing to split on: one list, everything in it, input order preserved.
    {
        const std::vector<BatchCandidate> candidates = {cubeAt(0.0f), cubeAt(1.0f), cubeAt(2.0f)};
        const auto lists = splitBatchLists(candidates, 0.0f, false);
        check(lists.size() == 1, "compatible instances form one list");
        check(lists[0] == std::vector<size_t>({0, 1, 2}), "input order is preserved");
    }

    // A different vertex layout cannot share the merged buffer; merged anyway, it
    // would be read through a reinterpret_cast as the wrong layout.
    {
        std::vector<BatchCandidate> candidates = {cubeAt(0.0f, 1u), cubeAt(1.0f, 2u), cubeAt(2.0f, 1u)};
        const auto lists = splitBatchLists(candidates, 0.0f, false);
        check(lists.size() == 2, "a second vertex layout opens a second list");
        check(lists[0] == std::vector<size_t>({0, 2}), "same-layout instances meet across the odd one");
        check(lists[1] == std::vector<size_t>({1}), "the odd layout gets its own list");
    }

    // The batch MeshInstance carries ONE castShadow flag, so mixing casters with
    // non-casters would silently give the whole batch the first one's shadow.
    {
        std::vector<BatchCandidate> candidates = {cubeAt(0.0f), cubeAt(1.0f), cubeAt(2.0f)};
        candidates[0].castShadow = true;
        candidates[1].castShadow = false;
        candidates[2].castShadow = true;
        const auto lists = splitBatchLists(candidates, 0.0f, false);
        check(lists.size() == 2, "castShadow splits the bucket");
        check(lists[0] == std::vector<size_t>({0, 2}), "the two casters batch together");
    }

    {
        std::vector<BatchCandidate> candidates = {cubeAt(0.0f), cubeAt(1.0f)};
        candidates[1].receiveShadow = true;
        const auto lists = splitBatchLists(candidates, 0.0f, false);
        check(lists.size() == 2, "receiveShadow splits the bucket");
    }

    // The merged mesh declares one primitive type and rebuilds its indices.
    {
        std::vector<BatchCandidate> candidates = {cubeAt(0.0f), cubeAt(1.0f)};
        candidates[1].primitiveType = 5;
        const auto lists = splitBatchLists(candidates, 0.0f, false);
        check(lists.size() == 2, "primitive type splits the bucket");
    }

    // maxAabbSize: five unit cubes spanning 5 units of X, capped at 2. A list may
    // reach a full extent of 2, which is two cubes.
    {
        std::vector<BatchCandidate> candidates;
        for (int i = 0; i < 5; ++i) {
            candidates.push_back(cubeAt(static_cast<float>(i)));
        }
        const auto lists = splitBatchLists(candidates, 2.0f, false);
        check(lists.size() == 3, "maxAabbSize 2 splits five unit cubes into three batches");
        check(lists[0] == std::vector<size_t>({0, 1}), "each batch spans at most the limit");
        check(listsCovering(lists) == 5, "no instance is dropped by the split");
    }

    // 0 means no limit — BatchGroup's default, and what every existing scene uses.
    {
        std::vector<BatchCandidate> candidates;
        for (int i = 0; i < 50; ++i) {
            candidates.push_back(cubeAt(static_cast<float>(i)));
        }
        const auto lists = splitBatchLists(candidates, 0.0f, false);
        check(lists.size() == 1, "maxAabbSize 0 means no limit");
    }

    // A dynamic batch gives every instance a matrix in the palette.
    {
        std::vector<BatchCandidate> candidates;
        for (size_t i = 0; i < kMaxDynamicBatchInstances + 10; ++i) {
            candidates.push_back(cubeAt(0.0f));
        }
        const auto staticLists = splitBatchLists(candidates, 0.0f, false);
        const auto dynamicLists = splitBatchLists(candidates, 0.0f, true);
        check(staticLists.size() == 1, "a static batch has no instance cap");
        check(dynamicLists.size() == 2, "a dynamic batch caps its instance count");
        check(dynamicLists[0].size() == kMaxDynamicBatchInstances, "the cap is the palette limit");
        check(listsCovering(dynamicLists) == candidates.size(), "the overflow is batched, not dropped");
    }

    check(splitBatchLists({}, 0.0f, false).empty(), "an empty bucket produces no lists");

    // Everything else a batch draws with once: the mask (which lights reach it), the
    // stencil and the draw bucket. A source that differs goes to another batch.
    {
        std::vector<BatchCandidate> candidates = {cubeAt(0.0f), cubeAt(1.0f), cubeAt(2.0f), cubeAt(3.0f)};
        candidates[1].mask = 2u;
        candidates[2].hasStencil = true;
        candidates[2].stencilFrontKey = 7u;
        candidates[2].stencilBackKey = 7u;
        candidates[3].drawBucket = 200;
        const auto lists = splitBatchLists(candidates, 0.0f, false);
        check(lists.size() == 4, "a different mask, stencil or draw bucket each start their own batch");

        candidates[3].drawBucket = candidates[0].drawBucket;
        candidates[3].mask = 2u;
        const auto regrouped = splitBatchLists(candidates, 0.0f, false);
        check(regrouped.size() == 3 && regrouped[1] == std::vector<size_t>{1, 3},
            "two sources with the same mask still meet");
    }

    // A blended bucket: taken in draw order, and a source is not merged past a skipped
    // one it overlaps unless the two share a draw order.
    {
        std::vector<BatchCandidate> candidates = {cubeAt(0.0f), cubeAt(5.0f), cubeAt(5.5f)};
        candidates[0].drawOrder = 1.0;
        candidates[1].drawOrder = 2.0;
        candidates[1].mask = 2u;   // skipped by the first sweep
        candidates[2].drawOrder = 3.0;
        const auto opaque = splitBatchLists(candidates, 0.0f, false);
        check(opaque.size() == 2 && opaque[0] == std::vector<size_t>{0, 2},
            "an opaque bucket merges past a skipped source");
        const auto blended = splitBatchLists(candidates, 0.0f, false, true);
        check(blended.size() == 3, "a blended one does not merge past the source it would draw over (" +
            std::to_string(blended.size()) + " lists)");

        candidates[1].drawOrder = 1.0;
        candidates[2].drawOrder = 1.0;
        const auto sameOrder = splitBatchLists(candidates, 0.0f, false, true);
        check(sameOrder.size() == 2 && sameOrder[0] == std::vector<size_t>{0, 2},
            "unless the two share a draw order");

        // Out of draw order in the bucket: taken in draw order.
        std::vector<BatchCandidate> shuffled = {cubeAt(0.0f), cubeAt(1.0f)};
        shuffled[0].drawOrder = 5.0;
        shuffled[1].drawOrder = 1.0;
        const auto ordered = splitBatchLists(shuffled, 0.0f, false, true);
        check(ordered.size() == 1 && ordered[0] == std::vector<size_t>{1, 0}, "a blended bucket is taken in draw order");
    }

    std::cout << "\nMerged triangle indices\n";

    // A mirrored source's winding is reversed as it is merged, for every index width
    // and for a source with no index buffer at all.
    {
        const uint8_t bytes8[] = {0, 1, 2, 2, 1, 3};
        std::vector<uint32_t> merged;
        appendTriangleIndices(merged, bytes8, 1, 6, 4, 10, false);
        check(merged == std::vector<uint32_t>{10, 11, 12, 12, 11, 13}, "8-bit indices are offset");

        const uint16_t idx16[] = {0, 1, 2, 2, 1, 3};
        merged.clear();
        appendTriangleIndices(merged, reinterpret_cast<const uint8_t*>(idx16), 2, 6, 4, 0, true);
        check(merged == std::vector<uint32_t>{0, 2, 1, 2, 3, 1}, "a mirrored source's triangles are re-wound");

        const uint32_t idx32[] = {4, 5, 6};
        merged = {1, 2, 3};
        appendTriangleIndices(merged, reinterpret_cast<const uint8_t*>(idx32), 4, 3, 7, 100, true);
        check(merged == std::vector<uint32_t>{1, 2, 3, 104, 106, 105},
            "only the appended source is re-wound, not what was merged before it");

        merged.clear();
        appendTriangleIndices(merged, nullptr, 0, 0, 3, 5, true);
        check(merged == std::vector<uint32_t>{5, 7, 6}, "an unindexed source gets re-wound identity indices");
    }

    std::cout << "\nVertex format batching hash\n";

    // The batching hash is the attribute SET: layout-independent, so two formats
    // that describe the same attributes at different offsets still batch together,
    // while a different attribute set does not.
    {
        const VertexFormat packed(56, VertexFormat::standardElements(), true, false);
        const VertexFormat skinned(88, VertexFormat::skinnedElements(), true, false);
        const VertexFormat points(28, VertexFormat::pointElements(), true, false);

        auto shuffled = VertexFormat::standardElements();
        std::swap(shuffled[0], shuffled[3]);
        const VertexFormat reordered(56, shuffled, true, false);

        check(packed.batchingHash() != skinned.batchingHash(),
            "a skinned layout does not batch with the packed one");
        check(packed.batchingHash() != points.batchingHash(),
            "a point layout does not batch with the packed one");
        check(packed.batchingHash() == reordered.batchingHash(),
            "declaration order does not change the batching hash");
        check(packed.renderingHash() != reordered.renderingHash(),
            "the rendering hash still distinguishes the two declarations");

        // The merge paths cast straight to the packed struct, so this is the
        // predicate that has to reject everything else.
        check(formatIsPackedVertexLayout(packed), "the packed layout is mergeable");
        check(!formatIsPackedVertexLayout(skinned), "a skinned layout is not mergeable");
        check(!formatIsPackedVertexLayout(points), "a point layout is not mergeable");
        check(!formatIsPackedVertexLayout(reordered),
            "the same attributes at different offsets are not mergeable");

        // Same attributes and offsets, padded stride: the cast would step over the
        // padding and read every vertex but the first from the wrong address.
        const VertexFormat padded(64, VertexFormat::standardElements(), true, false);
        check(!formatIsPackedVertexLayout(padded), "a padded stride is not mergeable");
    }

    return finish("batch split");
}
