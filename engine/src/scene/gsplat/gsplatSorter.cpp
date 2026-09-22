// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "gsplatSorter.h"
#include "gsplatSortKeys.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace visutwin::canvas
{
    namespace
    {
        constexpr int NUM_BINS = GSPLAT_SORT_BINS;
        constexpr int CHUNK_SIZE = 256;
    }

    GSplatSorter::GSplatSorter(std::vector<float> centers)
        : _centers(std::move(centers))
    {
        // Precompute per-chunk bounding spheres + overall bounds (upstream computes
        // these in the worker when no chunk data is provided).
        const size_t numVertices = _centers.size() / 3;
        const size_t numChunks = (numVertices + CHUNK_SIZE - 1) / CHUNK_SIZE;
        _chunks.resize(numChunks * 4);

        Vector3 boundMin(std::numeric_limits<float>::max());
        Vector3 boundMax(std::numeric_limits<float>::lowest());

        for (size_t c = 0; c < numChunks; ++c) {
            Vector3 chunkMin(std::numeric_limits<float>::max());
            Vector3 chunkMax(std::numeric_limits<float>::lowest());
            const size_t start = c * CHUNK_SIZE;
            const size_t end = std::min(numVertices, (c + 1) * CHUNK_SIZE);
            for (size_t i = start; i < end; ++i) {
                const Vector3 p = Vector3::load(&_centers[i * 3]);
                chunkMin = Vector3::min(chunkMin, p);
                chunkMax = Vector3::max(chunkMax, p);
            }
            ((chunkMin + chunkMax) * 0.5f).store(&_chunks[c * 4]);
            _chunks[c * 4 + 3] = 0.5f * (chunkMax - chunkMin).length();
            boundMin = Vector3::min(boundMin, chunkMin);
            boundMax = Vector3::max(boundMax, chunkMax);
        }
        _boundMin = boundMin;
        _boundMax = boundMax;

        _worker = std::thread([this] { workerLoop(); });
    }

    GSplatSorter::~GSplatSorter()
    {
        {
            std::lock_guard lock(_mutex);
            _quit = true;
        }
        _condition.notify_all();
        if (_worker.joinable()) {
            _worker.join();
        }
    }

    void GSplatSorter::setCamera(const Vector3& position, const Vector3& direction)
    {
        {
            std::lock_guard lock(_mutex);
            _requestPosition = position;
            _requestDirection = direction;
            _requestPending = true;
        }
        _condition.notify_one();
    }

    bool GSplatSorter::fetchResult(std::vector<uint32_t>& outOrder, uint32_t& outVisibleCount)
    {
        std::lock_guard lock(_mutex);
        if (!_resultReady) {
            return false;
        }
        outOrder.swap(_resultOrder);
        outVisibleCount = _resultVisibleCount;
        _resultReady = false;
        return true;
    }

    void GSplatSorter::workerLoop()
    {
        for (;;) {
            Vector3 position, direction;
            {
                std::unique_lock lock(_mutex);
                _condition.wait(lock, [this] { return _requestPending || _quit; });
                if (_quit) {
                    return;
                }
                position = _requestPosition;
                direction = _requestDirection;
                _requestPending = false;
            }

            // Skip when the camera barely moved (upstream epsilon check). Per component
            // rather than through maxComponent(), which reduces with std::max and would
            // drop a NaN lane: a NaN camera position must still reach the sort, as it did
            // when this was three explicit comparisons.
            constexpr float epsilon = 0.001f;
            const Vector3 movedPosition = (position - _lastPosition).abs();
            const Vector3 movedDirection = (direction - _lastDirection).abs();
            if (movedPosition.getX() < epsilon && movedPosition.getY() < epsilon &&
                movedPosition.getZ() < epsilon &&
                movedDirection.getX() < epsilon && movedDirection.getY() < epsilon &&
                movedDirection.getZ() < epsilon) {
                continue;
            }
            _lastPosition = position;
            _lastDirection = direction;

            sort(position, direction);
        }
    }

    void GSplatSorter::sort(const Vector3& position, const Vector3& direction)
    {
        const size_t numVertices = _centers.size() / 3;
        if (numVertices == 0) {
            return;
        }

        const float dx = direction.getX(), dy = direction.getY(), dz = direction.getZ();

        // Min/max projected distance over the bound corners. A corner picks min or max
        // per axis, so the extremes are the sums of the per-axis extreme products; the
        // rounding of a sum is monotone, so this is the same corner the full sweep finds.
        const Vector3 productMin = _boundMin * direction;
        const Vector3 productMax = _boundMax * direction;
        const Vector3 lowest = Vector3::min(productMax, productMin);
        const Vector3 highest = Vector3::max(productMax, productMin);
        const float minDist = lowest.getX() + lowest.getY() + lowest.getZ();
        const float maxDist = highest.getX() + highest.getY() + highest.getZ();

        const int compareBits = std::max(10, std::min(20,
            static_cast<int>(std::lround(std::log2(std::max(1.0, static_cast<double>(numVertices) / 4.0))))));
        const uint32_t bucketCount = (1u << compareBits) + 1u;

        _distances.resize(numVertices);
        _countBuffer.assign(bucketCount, 0u);
        _order.resize(numVertices);

        const float range = maxDist - minDist;
        if (range < 1e-6f) {
            for (size_t i = 0; i < numVertices; ++i) {
                _distances[i] = 0;
            }
            _countBuffer[0] = static_cast<uint32_t>(numVertices);
        } else {
            // Coarse histogram over chunks: distribute sort-key bits by density.
            int binCount[NUM_BINS] = {};
            const size_t numChunks = _chunks.size() / 4;
            for (size_t i = 0; i < numChunks; ++i) {
                const float r = _chunks[i * 4 + 3];
                const float d = Vector3::load(&_chunks[i * 4]).dot(direction) - minDist;
                const int binMin = std::max(0, static_cast<int>(std::floor((d - r) * NUM_BINS / range)));
                const int binMax = std::min(NUM_BINS, static_cast<int>(std::ceil((d + r) * NUM_BINS / range)));
                for (int j = binMin; j < binMax; ++j) {
                    binCount[j]++;
                }
            }
            int binTotal = 0;
            for (const int count : binCount) binTotal += count;

            uint32_t binDivider[NUM_BINS];
            uint32_t binBase[NUM_BINS];
            for (int i = 0; i < NUM_BINS; ++i) {
                binDivider[i] = binTotal > 0
                    ? static_cast<uint32_t>(static_cast<double>(binCount[i]) / binTotal * bucketCount) : 0u;
            }
            for (int i = 0; i < NUM_BINS; ++i) {
                binBase[i] = i == 0 ? 0u : binBase[i - 1] + binDivider[i - 1];
            }

            // The per-splat depth and key, in 4-lane SIMD where the target has it
            // (gsplatSortKeys.h). The count is a separate pass: it scatters writes
            // across the buckets, which is what kept the combined loop scalar.
            GSplatSortKeyParams keyParams;
            keyParams.dx = dx;
            keyParams.dy = dy;
            keyParams.dz = dz;
            keyParams.minDist = minDist;
            keyParams.invBinRange = static_cast<float>(NUM_BINS) / range;
            keyParams.binBase = binBase;
            keyParams.binDivider = binDivider;
            keyParams.bucketCount = bucketCount;
            computeGSplatSortKeys(_centers.data(), numVertices, keyParams, _distances.data());
            for (size_t i = 0; i < numVertices; ++i) {
                _countBuffer[_distances[i]]++;
            }
        }

        // Prefix sum, then scatter — ascending distance along the view direction.
        for (uint32_t i = 1; i < bucketCount; ++i) {
            _countBuffer[i] += _countBuffer[i - 1];
        }
        for (size_t i = 0; i < numVertices; ++i) {
            _order[--_countBuffer[_distances[i]]] = static_cast<uint32_t>(i);
        }

        // Count splats behind the camera (projected distance < camera distance):
        // they occupy the front of the ascending order.
        const float cameraDist = position.dot(direction);
        const auto distAt = [&](const size_t i) {
            return Vector3::load(&_centers[static_cast<size_t>(_order[i]) * 3]).dot(direction) - cameraDist;
        };
        size_t behindCount = 0;
        {
            size_t lo = 0, hi = numVertices;
            while (lo < hi) {
                const size_t mid = (lo + hi) / 2;
                if (distAt(mid) < 0.0f) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            behindCount = lo;
        }

        // Publish reversed (farthest first); visible count trims behind-camera splats.
        {
            std::lock_guard lock(_mutex);
            _resultOrder.resize(numVertices);
            for (size_t i = 0; i < numVertices; ++i) {
                _resultOrder[i] = _order[numVertices - 1 - i];
            }
            _resultVisibleCount = static_cast<uint32_t>(numVertices - behindCount);
            _resultReady = true;
        }
    }
}
