// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2026
//
#include "batchSplit.h"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <utility>

namespace visutwin::canvas
{
    namespace
    {
        // Would merging `candidate` into `current` push any dimension of the batch past the limit?
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

        // Everything one batch carries exactly once, so two candidates that differ in
        // any of it cannot share a draw.
        bool sharesBatchState(const BatchCandidate& a, const BatchCandidate& b)
        {
            return a.formatBatchingHash == b.formatBatchingHash &&
                a.primitiveType == b.primitiveType &&
                a.castShadow == b.castShadow &&
                a.receiveShadow == b.receiveShadow &&
                a.mask == b.mask &&
                a.hasStencil == b.hasStencil &&
                a.stencilFrontKey == b.stencilFrontKey &&
                a.stencilBackKey == b.stencilBackKey &&
                a.drawBucket == b.drawBucket;
        }

        // Touching counts as overlapping, as a closed-interval AABB test does.
        bool boxesOverlap(const BoundingBox& a, const BoundingBox& b)
        {
            const Vector3 gap = (a.center() - b.center()).abs() - (a.halfExtents() + b.halfExtents());
            return gap.maxComponent() <= 0.0f;
        }
    }

    std::vector<std::vector<size_t>> splitBatchLists(const std::vector<BatchCandidate>& candidates,
        const float maxAabbSize, const bool dynamic, const bool translucent)
    {
        std::vector<std::vector<size_t>> lists;
        if (candidates.empty()) {
            return lists;
        }

        std::vector<size_t> remaining(candidates.size());
        std::iota(remaining.begin(), remaining.end(), size_t{0});
        if (translucent) {
            // Stable: equal draw orders keep the caller's order.
            std::stable_sort(remaining.begin(), remaining.end(), [&candidates](const size_t a, const size_t b) {
                return candidates[a].drawOrder < candidates[b].drawOrder;
            });
        }

        while (!remaining.empty()) {
            const BatchCandidate& first = candidates[remaining[0]];

            std::vector<size_t> list{remaining[0]};
            std::vector<size_t> leftovers;
            BoundingBox aabb = first.aabb;
            // Translucent only: the union of the candidates skipped so far in this
            // sweep, which a later candidate may not be merged past if it overlaps it.
            bool anySkipped = false;
            BoundingBox skippedAabb;
            const auto skip = [&](const size_t index) {
                if (translucent) {
                    if (anySkipped) {
                        skippedAabb.add(candidates[index].aabb);
                    } else {
                        skippedAabb = candidates[index].aabb;
                        anySkipped = true;
                    }
                }
                leftovers.push_back(index);
            };

            for (size_t i = 1; i < remaining.size(); ++i) {
                const size_t index = remaining[i];
                const BatchCandidate& candidate = candidates[index];

                // A full dynamic batch takes no more instances; everything after it
                // goes to the next list untested.
                if (dynamic && list.size() >= kMaxDynamicBatchInstances) {
                    leftovers.insert(leftovers.end(), remaining.begin() + static_cast<long>(i),
                        remaining.end());
                    break;
                }

                if (!sharesBatchState(first, candidate)) {
                    skip(index);
                    continue;
                }

                BoundingBox merged;
                if (!fitsAabb(aabb, candidate.aabb, maxAabbSize, merged)) {
                    skip(index);
                    continue;
                }

                if (translucent && anySkipped && candidate.drawOrder != first.drawOrder &&
                    boxesOverlap(skippedAabb, candidate.aabb)) {
                    skip(index);
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

    void appendTriangleIndices(std::vector<uint32_t>& merged, const uint8_t* indices, const int indexSize,
        const int indexCount, const int vertexCount, const uint32_t vertexOffset, const bool reverseWinding)
    {
        const size_t start = merged.size();
        if (indices) {
            for (int i = 0; i < indexCount; ++i) {
                uint32_t index = 0;
                switch (indexSize) {
                case 1:
                    index = indices[i];
                    break;
                case 2: {
                    uint16_t value = 0;
                    std::memcpy(&value, indices + static_cast<size_t>(i) * 2, sizeof(value));
                    index = value;
                    break;
                }
                default:
                    std::memcpy(&index, indices + static_cast<size_t>(i) * 4, sizeof(index));
                    break;
                }
                merged.push_back(index + vertexOffset);
            }
        } else {
            for (int i = 0; i < vertexCount; ++i) {
                merged.push_back(vertexOffset + static_cast<uint32_t>(i));
            }
        }
        if (reverseWinding) {
            // Corners 1 and 2 of every whole triangle; a trailing partial triangle
            // (a malformed source) is left as it came.
            for (size_t i = start; i + 2 < merged.size(); i += 3) {
                std::swap(merged[i + 1], merged[i + 2]);
            }
        }
    }

    bool entityIsBatchable(const std::vector<bool>& instanceDeforms)
    {
        return std::ranges::none_of(instanceDeforms, [](const bool deforms) { return deforms; });
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
