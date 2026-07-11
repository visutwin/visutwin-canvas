// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <core/math/vector3.h>

namespace visutwin::canvas
{
    /**
     * Background depth sorter for Gaussian splats — a port of upstream's
     * gsplat-sort-worker.js (web worker) to a std::thread.
     *
     * Counting sort over view-direction-projected distances, with a coarse chunk
     * histogram distributing sort-key bits where the splats actually are.
     * DEVIATION: the order written for the GPU is reversed (farthest first) and
     * splats behind the camera are trimmed off via the instance count, so the
     * vertex shader can index `order[instance_id]` directly for back-to-front
     * blending.
     */
    class GSplatSorter
    {
    public:
        /** `centers` holds xyz per splat in model space (copied). */
        explicit GSplatSorter(std::vector<float> centers);
        ~GSplatSorter();

        /** Post the latest camera (in splat model space); wakes the worker. */
        void setCamera(const Vector3& position, const Vector3& direction);

        /**
         * Fetch the newest sort result if one is ready. `outOrder` receives one
         * uint32 per splat, farthest first; `outVisibleCount` excludes splats
         * behind the camera. Returns false when no new result is available.
         */
        bool fetchResult(std::vector<uint32_t>& outOrder, uint32_t& outVisibleCount);

    private:
        void workerLoop();
        void sort(const Vector3& position, const Vector3& direction);

        std::vector<float> _centers;
        std::vector<float> _chunks;      // center xyz + radius per 256-splat chunk
        Vector3 _boundMin;
        Vector3 _boundMax;

        std::thread _worker;
        std::mutex _mutex;
        std::condition_variable _condition;

        // Request state (guarded by _mutex).
        Vector3 _requestPosition{0.0f};
        Vector3 _requestDirection{0.0f, 0.0f, 1.0f};
        bool _requestPending = false;
        bool _quit = false;

        // Result state (guarded by _mutex).
        std::vector<uint32_t> _resultOrder;
        uint32_t _resultVisibleCount = 0;
        bool _resultReady = false;

        // Worker-local scratch.
        std::vector<uint32_t> _distances;
        std::vector<uint32_t> _countBuffer;
        std::vector<uint32_t> _order;
        Vector3 _lastPosition{1e30f};
        Vector3 _lastDirection{0.0f};
    };
}
