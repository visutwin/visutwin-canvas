// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// GpuProfiler::publishTimings is what makes the HUD's GPU figure mean anything.
// On a pipelined GPU a pass's own start-to-end interval contains the time it
// overlapped its neighbours, so summing intervals counts the same time twice; and
// a pass that waits for the drawable has that wait between the previous pass's end
// and its own start, so the end-to-end delta swallows a whole vsync interval. The
// resolver takes the SMALLER of the two per pass, which is right in both cases, and
// anchors the frame's first pass to the previous frame's last end.
//
// The case in "two drawable passes" is the one that was wrong until 2026-09-22,
// when a drawable-bound pass kept its own interval unconditionally: two such passes
// in a frame overlap, and the frame counted that time twice — `ambient-occlusion`
// zoomed in read 24-33 ms of GPU for an 8.3 ms frame.
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
        const bool valid = true)
    {
        Probe::RawPass raw;
        raw.name = name;
        raw.start = start;
        raw.end = end;
        raw.valid = valid;
        return raw;
    }
}

int main()
{
    // Each case gets its own probe: the resolver now carries an anchor from one
    // published frame to the next, so sharing one would couple the cases.
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

    // A pass after a 100-tick wait for the drawable: its delta would be 103, its own
    // interval is 3, and only the 3 is work.
    {
        Probe fresh;
        fresh.publishTimings({pass("scene", 0, 10), pass("compose", 110, 113)}, 1.0);
        check(near(fresh.passTimings()[1].milliseconds, 3.0), "a pass that waited keeps its own interval");
        check(near(fresh.frameMilliseconds(), 13.0), "the drawable wait is not in the frame");
    }

    // TWO passes targeting the drawable, overlapping each other as they do in a real
    // frame: compose runs 100..108 and the pass after it 101..109. Their own intervals
    // are 8 each, but only the 1 tick by which the second outlasts the first is its own
    // work. Summing intervals here is what read 30 ms for an 8.3 ms frame.
    {
        Probe fresh;
        fresh.publishTimings({pass("scene", 0, 10), pass("compose", 100, 108), pass("overlay", 101, 109)}, 1.0);
        check(near(fresh.passTimings()[1].milliseconds, 8.0), "the first drawable pass keeps its interval");
        check(near(fresh.passTimings()[2].milliseconds, 1.0), "the second costs only what it adds");
        check(near(fresh.frameMilliseconds(), 19.0), "two overlapping drawable passes are not counted twice");
    }

    // The frame's first pass is measured against the previous frame's last end, so an
    // interval that spans the neighbouring frame's work does not become its cost.
    {
        Probe fresh;
        fresh.publishTimings({pass("a", 0, 10)}, 1.0);
        fresh.publishTimings({pass("prepass", 5, 12), pass("b", 11, 14)}, 1.0);
        check(near(fresh.passTimings()[0].milliseconds, 2.0),
            "the first pass costs its delta from the previous frame's end");
        check(near(fresh.frameMilliseconds(), 4.0), "and the frame follows from it");
    }

    // An anchor from a frame the GPU finished long ago only makes the delta larger, so
    // the minimum keeps the interval: an idle gap between frames cannot inflate a frame.
    {
        Probe fresh;
        fresh.publishTimings({pass("a", 0, 10)}, 1.0);
        fresh.publishTimings({pass("prepass", 1000, 1004)}, 1.0);
        check(near(fresh.frameMilliseconds(), 4.0), "an idle gap between frames is not charged to it");
    }

    // A pass whose samples the GPU could not take reads 0 and does not move the
    // reference end: the next delta is measured from the last VALID end. "c" overlaps
    // "a", so its delta (6) is the smaller measurement and the one that must be used —
    // had the invalid pass moved the reference, the delta would have been 16.
    probe = Probe{};
    probe.publishTimings({pass("a", 0, 10), pass("lost", 0, 0, false), pass("c", 6, 16)}, 1.0);
    check(near(probe.passTimings()[1].milliseconds, 0.0), "an invalid pass reads zero");
    check(near(probe.passTimings()[2].milliseconds, 6.0), "the delta after it runs from the last valid end");

    // A gap between two passes is idle, not work: the pass costs its own interval.
    probe = Probe{};
    probe.publishTimings({pass("a", 0, 10), pass("c", 12, 16)}, 1.0);
    check(near(probe.passTimings()[1].milliseconds, 4.0), "a pass after an idle gap costs its interval");
    check(near(probe.frameMilliseconds(), 14.0), "the gap is not in the frame");

    // Ticks convert through the per-tick factor, and an end that precedes the
    // previous end (out-of-order samples) costs nothing rather than wrapping.
    probe = Probe{};
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
