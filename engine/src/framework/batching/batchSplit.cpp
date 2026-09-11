// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "batchSplit.h"

namespace visutwin::canvas
{
    namespace
    {
        // Would merging `candidate` into `current` push any dimension of the batch
        // past the limit? Upstream compares HALF extents against half the limit,
        // which is the same test written on the other side of the factor of two.
        bool fitsAabb(const BoundingBox& current, const BoundingBox& candidate,
            const float maxAabbSize, BoundingBox& merged)
        {
            merged = current;
            merged.add(candidate);
            if (maxAabbSize <= 0.0f) {
                return true;   // no limit
            }
            const float halfMax = maxAabbSize * 0.5f;
            const Vector3& half = merged.halfExtents();
            return half.getX() <= halfMax && half.getY() <= halfMax && half.getZ() <= halfMax;
        }
    }

    std::vector<std::vector<size_t>> splitBatchLists(const std::vector<BatchCandidate>& candidates,
        const float maxAabbSize, const bool dynamic)
    {
        std::vector<std::vector<size_t>> lists;
        if (candidates.empty()) {
            return lists;
        }

        std::vector<size_t> remaining(candidates.size());
        for (size_t i = 0; i < candidates.size(); ++i) {
            remaining[i] = i;
        }

        while (!remaining.empty()) {
            const BatchCandidate& first = candidates[remaining[0]];

            std::vector<size_t> list{remaining[0]};
            std::vector<size_t> leftovers;
            BoundingBox aabb = first.aabb;

            for (size_t i = 1; i < remaining.size(); ++i) {
                const size_t index = remaining[i];
                const BatchCandidate& candidate = candidates[index];

                // A full dynamic batch takes no more instances; everything after it
                // goes to the next list untested, as upstream does.
                if (dynamic && list.size() >= kMaxDynamicBatchInstances) {
                    leftovers.insert(leftovers.end(), remaining.begin() + static_cast<long>(i),
                        remaining.end());
                    break;
                }

                if (candidate.formatBatchingHash != first.formatBatchingHash ||
                    candidate.primitiveType != first.primitiveType ||
                    candidate.castShadow != first.castShadow ||
                    candidate.receiveShadow != first.receiveShadow) {
                    leftovers.push_back(index);
                    continue;
                }

                BoundingBox merged;
                if (!fitsAabb(aabb, candidate.aabb, maxAabbSize, merged)) {
                    leftovers.push_back(index);
                    continue;
                }

                aabb = merged;
                list.push_back(index);
            }

            lists.push_back(std::move(list));
            remaining = std::move(leftovers);
        }

        return lists;
    }

    bool formatIsPackedVertexLayout(const VertexFormat& format)
    {
        // The rendering hash pins every field the cast depends on — stride,
        // interleaving, and each element's semantic, type, component count and
        // OFFSET — so one comparison covers the whole layout. The batching hash
        // deliberately would not: it ignores offsets, which is what makes it the
        // right key for grouping and the wrong one for a reinterpret_cast.
        static const VertexFormat packed(kPackedVertexStride,
            VertexFormat::standardElements(), true, false);
        return format.renderingHash() == packed.renderingHash();
    }
}
