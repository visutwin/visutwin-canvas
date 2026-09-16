// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// GpuProfiler::publishTimings is what makes the HUD's GPU figure mean the same
// thing as upstream's. On a pipelined GPU a pass's own start-to-end interval
// contains the time it spent queued behind its predecessor, so summing intervals
// overestimates (upstream says so and reports a span instead); on a native
// swapchain the last pass also waits for the drawable inside the frame, so a
// span is the vsync interval. The resolver takes each pass as the delta between
// consecutive END samples and lets a drawable-bound pass keep its own interval.
// Those two rules are pinned here on synthetic samples.
//
#include <cmath>
#include <cstdio>
#include <vector>

#include "platform/graphics/gpuProfiler.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        if (!condition) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    }

    bool near(const double a, const double b)
    {
        return std::fabs(a - b) < 1e-9;
    }

    class Probe final : public GpuProfiler
    {
    public:
        using GpuProfiler::publishTimings;
        using GpuProfiler::RawPass;
    };

    Probe::RawPass pass(const char* name, const uint64_t start, const uint64_t end,
        const bool backBuffer = false, const bool valid = true)
    {
        Probe::RawPass raw;
        raw.name = name;
        raw.start = start;
        raw.end = end;
        raw.valid = valid;
        raw.backBuffer = backBuffer;
        return raw;
    }
}

int main()
{
    Probe probe;

    // Three offscreen passes whose vertex work starts before the previous pass's
    // fragment work ends. Own intervals: 10, 12, 12 (sum 34). End deltas: 10, 5, 5
    // (sum 20), which is also the span from the first end back to the first start
    // to the last end — no interval is counted twice.
    probe.publishTimings({pass("a", 0, 10), pass("b", 3, 15), pass("c", 8, 20)}, 1.0);
    check(probe.passTimings().size() == 3, "three passes published");
    check(near(probe.passTimings()[0].milliseconds, 10.0), "first pass keeps its own interval");
    check(near(probe.passTimings()[1].milliseconds, 5.0), "an overlapping pass costs its end delta");
    check(near(probe.passTimings()[2].milliseconds, 5.0), "and so does the next");
    check(near(probe.frameMilliseconds(), 20.0), "the frame is the sum of the deltas, not of the intervals");

    // A back-buffer pass after a 100-tick wait for the drawable: its delta would
    // be 103, its own interval is 3, and only the 3 is work.
    probe.publishTimings({pass("scene", 0, 10), pass("compose", 110, 113, true)}, 1.0);
    check(near(probe.passTimings()[1].milliseconds, 3.0), "a drawable-bound pass keeps its own interval");
    check(near(probe.frameMilliseconds(), 13.0), "the drawable wait is not in the frame");

    // A pass whose samples the GPU could not take reads 0 and does not move the
    // reference end: the next delta is measured from the last VALID end.
    probe.publishTimings({pass("a", 0, 10), pass("lost", 0, 0, false, false), pass("c", 12, 16)}, 1.0);
    check(near(probe.passTimings()[1].milliseconds, 0.0), "an invalid pass reads zero");
    check(near(probe.passTimings()[2].milliseconds, 6.0), "the delta after it runs from the last valid end");

    // Ticks convert through the per-tick factor, and an end that precedes the
    // previous end (out-of-order samples) costs nothing rather than wrapping.
    probe.publishTimings({pass("a", 0, 1000), pass("b", 500, 900)}, 0.001);
    check(near(probe.passTimings()[0].milliseconds, 1.0), "ticks convert through the factor");
    check(near(probe.passTimings()[1].milliseconds, 0.0), "an end before the previous end costs nothing");

    if (failures == 0) {
        std::printf("gpu-profiler-timing: all checks passed\n");
        return 0;
    }
    std::printf("gpu-profiler-timing: %d check(s) failed\n", failures);
    return 1;
}
