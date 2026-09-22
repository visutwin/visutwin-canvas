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
        };

        /**
         * Turns raw samples into passTimings() and frameMilliseconds().
         *
         * A pass costs the SMALLER of two measurements: its own start-to-end interval,
         * and the delta from the previous pass's end to its own end. Neither alone is
         * right, and which one is too big depends on what the GPU was doing:
         *
         *  - Overlap. A tiler starts a pass's vertex work while its predecessor's
         *    fragment work still runs, so own intervals overlap and summing them counts
         *    the same time several times over. The end-to-end delta is that pass's share
         *    of a serial timeline, so it is the smaller and the right one.
         *  - Waiting. A pass targeting the drawable begins only once the display releases
         *    one, and that wait sits between the previous pass's end and this pass's
         *    start. The delta swallows the wait (a whole vsync interval); the own
         *    interval is the smaller and the right one.
         *
         * The minimum picks the correct measurement in both cases without having to know
         * which case it is. Until 2026-09-22 a pass kept its own interval whenever it
         * targeted the drawable, which broke as soon as a frame had TWO such passes:
         * their intervals overlap each other almost entirely, so the frame counted that
         * time twice, and `ambient-occlusion` zoomed in reported 24-33 ms of GPU for an
         * 8.3 ms frame at 120 fps while nothing was actually slow.
         *
         * The frame's first pass is measured against the LAST end of the previous frame,
         * for the same reason: with no anchor it could only keep its own interval, which
         * on a pipelined GPU spans neighbouring frames' work (measured: 9.8 ms against a
         * serial cost near 1 ms). An anchor from a frame the GPU finished long ago can
         * only make the delta larger, and the minimum then keeps the interval, so the
         * anchor cannot inflate anything.
         *
         * What this cannot do is separate a drawable wait that falls INSIDE a pass's own
         * interval; a frame that outruns the display still carries that wait.
         */
        void publishTimings(const std::vector<RawPass>& passes, double millisecondsPerTick);

        bool _enabled = false;
        std::vector<PassTiming> _passTimings;
        double _frameMilliseconds = 0.0;

        // Last valid end sample of the previous published frame: the anchor the next
        // frame's first pass is measured against.
        uint64_t _previousFrameEnd = 0;
        bool _havePreviousFrameEnd = false;
    };
}
