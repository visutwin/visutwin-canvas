// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "gpuProfiler.h"

#include <algorithm>

namespace visutwin::canvas
{
    void GpuProfiler::publishTimings(const std::vector<RawPass>& passes, const double millisecondsPerTick)
    {
        _passTimings.clear();
        _frameMilliseconds = 0.0;

        // The previous frame's last end anchors this frame's first pass (see the header).
        bool havePreviousEnd = _havePreviousFrameEnd;
        uint64_t previousEnd = _previousFrameEnd;

        for (const auto& pass : passes) {
            if (!pass.valid || pass.end < pass.start) {
                _passTimings.push_back({pass.name, 0.0});
                continue;
            }

            // Its own interval, and — when there is an earlier end to measure from — its
            // share of the serial timeline. The smaller one is the pass's work: the delta
            // is inflated by any wait before the pass, the interval by any overlap with
            // its neighbours.
            uint64_t ticks = pass.end - pass.start;
            if (havePreviousEnd) {
                const uint64_t delta = pass.end >= previousEnd ? pass.end - previousEnd : 0;
                ticks = std::min(ticks, delta);
            }

            const double ms = static_cast<double>(ticks) * millisecondsPerTick;
            _passTimings.push_back({pass.name, ms});
            _frameMilliseconds += ms;
            previousEnd = pass.end;
            havePreviousEnd = true;
        }

        _previousFrameEnd = previousEnd;
        _havePreviousFrameEnd = havePreviousEnd;
    }
}
