// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.09.2025.
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace visutwin::canvas
{
    /**
     * GPU pass timing profiler. When enabled, the graphics backend samples GPU
     * timestamps around every render pass and resolves them a few frames later
     * (results lag GPU execution by ~2 frames).
     *
     * Mirrors upstream gpu-profiler.js; the Metal backend samples at stage
     * boundaries with MTLCounterSampleBuffer, Vulkan brackets each pass with
     * timestamp queries. Both hand their raw samples to publishTimings(), which
     * owns the one thing that makes the figure comparable with upstream's — see
     * there.
     */
    class GpuProfiler
    {
    public:
        struct PassTiming
        {
            std::string name;
            double milliseconds = 0.0;
        };

        virtual ~GpuProfiler() = default;

        /** Enable/disable sampling (disabled by default — sampling has a small cost). */
        void setEnabled(const bool value) { _enabled = value; }
        bool enabled() const { return _enabled; }

        /** Per-pass GPU times of the most recently resolved frame. */
        const std::vector<PassTiming>& passTimings() const { return _passTimings; }

        /** Total GPU time of the most recently resolved frame (sum of pass times). */
        double frameMilliseconds() const { return _frameMilliseconds; }

    protected:
        /** One pass's raw samples, in submission order. */
        struct RawPass
        {
            std::string name;
            uint64_t start = 0;       // first sample (start of the pass's vertex work)
            uint64_t end = 0;         // last sample (end of its fragment work)
            bool valid = false;       // both samples were taken
            bool backBuffer = false;  // targets the swapchain / drawable
        };

        /**
         * Turns raw samples into passTimings() and frameMilliseconds().
         *
         * A pass's own start-to-end interval is NOT its cost on a pipelined GPU: the
         * tiler starts a pass's vertex work while the previous pass's fragment work
         * is still running, so the intervals overlap and their sum overestimates —
         * upstream's profiler says exactly that, and reports the span from the first
         * start to the last end instead. That span is no use on a native swapchain
         * either: the last pass waits for the display's drawable INSIDE the frame's
         * GPU timeline, so the span is the whole vsync interval (measured: 16 ms at
         * 60 Hz for 3 ms of work). What is comparable is the pass's cost as the
         * delta between consecutive END samples, which are serial on both backends,
         * except for a pass that targets the drawable, whose delta would span that
         * wait: it keeps its own interval. The frame is the sum of those. A browser
         * never has the wait in its command stream, which is why upstream's span
         * and this sum measure the same thing.
         *
         * A pass that outruns the display (vsync off) can still see the drawable
         * wait inside its own interval; that is a benchmark configuration and the
         * back-buffer rows are where it shows.
         */
        void publishTimings(const std::vector<RawPass>& passes, double millisecondsPerTick);

        bool _enabled = false;
        std::vector<PassTiming> _passTimings;
        double _frameMilliseconds = 0.0;
    };
}
