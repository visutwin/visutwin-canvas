// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "gsplatSorter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace visutwin::canvas
{
    namespace
    {
        constexpr int NUM_BINS = 32;
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

        float bminX = std::numeric_limits<float>::max(), bminY = bminX, bminZ = bminX;
        float bmaxX = std::numeric_limits<float>::lowest(), bmaxY = bmaxX, bmaxZ = bmaxX;

        for (size_t c = 0; c < numChunks; ++c) {
            float mx = std::numeric_limits<float>::max(), my = mx, mz = mx;
            float Mx = std::numeric_limits<float>::lowest(), My = Mx, Mz = Mx;
            const size_t start = c * CHUNK_SIZE;
            const size_t end = std::min(numVertices, (c + 1) * CHUNK_SIZE);
            for (size_t i = start; i < end; ++i) {
                const float x = _centers[i * 3 + 0];
                const float y = _centers[i * 3 + 1];
                const float z = _centers[i * 3 + 2];
                mx = std::min(mx, x); Mx = std::max(Mx, x);
                my = std::min(my, y); My = std::max(My, y);
                mz = std::min(mz, z); Mz = std::max(Mz, z);
            }
            _chunks[c * 4 + 0] = (mx + Mx) * 0.5f;
            _chunks[c * 4 + 1] = (my + My) * 0.5f;
            _chunks[c * 4 + 2] = (mz + Mz) * 0.5f;
            _chunks[c * 4 + 3] = 0.5f * std::sqrt((Mx - mx) * (Mx - mx) +
                (My - my) * (My - my) + (Mz - mz) * (Mz - mz));
            bminX = std::min(bminX, mx); bmaxX = std::max(bmaxX, Mx);
            bminY = std::min(bminY, my); bmaxY = std::max(bmaxY, My);
            bminZ = std::min(bminZ, mz); bmaxZ = std::max(bmaxZ, Mz);
        }
        _boundMin = Vector3(bminX, bminY, bminZ);
        _boundMax = Vector3(bmaxX, bmaxY, bmaxZ);

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

            // Skip when the camera barely moved (upstream epsilon check).
            constexpr float epsilon = 0.001f;
            if (std::abs(position.getX() - _lastPosition.getX()) < epsilon &&
                std::abs(position.getY() - _lastPosition.getY()) < epsilon &&
                std::abs(position.getZ() - _lastPosition.getZ()) < epsilon &&
                std::abs(direction.getX() - _lastDirection.getX()) < epsilon &&
                std::abs(direction.getY() - _lastDirection.getY()) < epsilon &&
                std::abs(direction.getZ() - _lastDirection.getZ()) < epsilon) {
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

        // Min/max projected distance over the bound corners.
        float minDist = 0.0f, maxDist = 0.0f;
        for (int i = 0; i < 8; ++i) {
            const float x = (i & 1) ? _boundMin.getX() : _boundMax.getX();
            const float y = (i & 2) ? _boundMin.getY() : _boundMax.getY();
            const float z = (i & 4) ? _boundMin.getZ() : _boundMax.getZ();
            const float d = x * dx + y * dy + z * dz;
            if (i == 0) {
                minDist = maxDist = d;
            } else {
                minDist = std::min(minDist, d);
                maxDist = std::max(maxDist, d);
            }
        }

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
                const float x = _chunks[i * 4 + 0];
                const float y = _chunks[i * 4 + 1];
                const float z = _chunks[i * 4 + 2];
                const float r = _chunks[i * 4 + 3];
                const float d = x * dx + y * dy + z * dz - minDist;
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

            const float binRange = range / NUM_BINS;
            for (size_t i = 0; i < numVertices; ++i) {
                const float x = _centers[i * 3 + 0];
                const float y = _centers[i * 3 + 1];
                const float z = _centers[i * 3 + 2];
                const float d = (x * dx + y * dy + z * dz - minDist) / binRange;
                const int bin = std::clamp(static_cast<int>(d), 0, NUM_BINS - 1);
                const uint32_t sortKey = std::min(bucketCount - 1u,
                    binBase[bin] + static_cast<uint32_t>(binDivider[bin] * (d - static_cast<float>(bin))));
                _distances[i] = sortKey;
                _countBuffer[sortKey]++;
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
        const float cameraDist = position.getX() * dx + position.getY() * dy + position.getZ() * dz;
        const auto distAt = [&](const size_t i) {
            const size_t o = static_cast<size_t>(_order[i]) * 3;
            return _centers[o] * dx + _centers[o + 1] * dy + _centers[o + 2] * dz - cameraDist;
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
