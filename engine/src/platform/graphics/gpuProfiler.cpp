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

        bool havePreviousEnd = false;
        uint64_t previousEnd = 0;
        for (const auto& pass : passes) {
            if (!pass.valid || pass.end < pass.start) {
                _passTimings.push_back({pass.name, 0.0});
                continue;
            }
            uint64_t ticks;
            if (havePreviousEnd && !pass.backBuffer) {
                // End-to-end delta: the pass's own work, not the queueing overlap.
                ticks = pass.end >= previousEnd ? pass.end - previousEnd : 0;
            } else {
                // The frame's first pass, or one targeting the drawable: its own
                // interval (see the header for why the delta is wrong here).
                ticks = pass.end - pass.start;
            }
            const double ms = static_cast<double>(ticks) * millisecondsPerTick;
            _passTimings.push_back({pass.name, ms});
            _frameMilliseconds += ms;
            previousEnd = pass.end;
            havePreviousEnd = true;
        }
    }
}
