// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Batching merges many mesh instances into one draw, which means one vertex
// layout, one primitive type, one pair of shadow flags and one bounding box. The
// batcher used to apply none of those rules: it reinterpreted any vertex buffer
// as the 56-byte packed layout and never read BatchGroup::maxAabbSize at all, so
// a mesh with a different stride merged as garbage and one batch could span the
// whole scene. These hold the splitting rules that replaced that.

#include <iostream>
#include <vector>

#include "core/shape/boundingBox.h"
#include "framework/batching/batchSplit.h"
#include "platform/graphics/vertexFormat.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

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
        // Upstream excludes the WHOLE entity when any of its mesh instances deforms.
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

    // A different vertex layout cannot share the merged buffer. This is the case
    // that used to be merged through a reinterpret_cast.
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

    std::cout << (failures == 0 ? "\nAll batch split tests passed\n"
                                : "\nBatch split tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
