// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.09.2025.
//

#pragma once

#include <string>
#include <vector>

namespace visutwin::canvas
{
    /**
     * GPU pass timing profiler. When enabled, the graphics backend samples GPU
     * timestamps around every render pass and resolves them a few frames later
     * (results lag GPU execution by ~2 frames).
     *
     * Mirrors upstream gpu-profiler.js in spirit; the Metal backend implements it
     * with MTLCounterSampleBuffer stage-boundary sampling.
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
        bool _enabled = false;
        std::vector<PassTiming> _passTimings;
        double _frameMilliseconds = 0.0;
    };
}
