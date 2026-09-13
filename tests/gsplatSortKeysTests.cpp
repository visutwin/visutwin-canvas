// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The splat sort key kernel has a SIMD path and a scalar reference, and the two
// must agree BIT FOR BIT: a key that differs by one bucket reorders splats, and
// nothing in a render would say which order was intended. This compares them on
// every block-size remainder (the SIMD loops handle four splats at a time and
// hand the tail to the scalar path, and the SSE path stops one block early
// because its loads read one float past a block) and on depths outside the bin
// range, which is where the old sorter's unclamped conversions went wrong.
//
// It also checks the one property the sort relies on regardless of backend:
// keys never decrease as depth increases.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "scene/gsplat/gsplatSortKeys.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        if (!condition) {
            std::printf("FAIL: %s\n", what.c_str());
            ++failures;
        }
    }

    // Valid density bins in the shape GSplatSorter builds: dividers that sum to at
    // most bucketCount, bases as their running sum.
    struct Bins
    {
        std::vector<uint32_t> base = std::vector<uint32_t>(GSPLAT_SORT_BINS);
        std::vector<uint32_t> divider = std::vector<uint32_t>(GSPLAT_SORT_BINS);
        uint32_t bucketCount = 0;
    };

    Bins makeBins(std::mt19937& rng, const uint32_t bucketCount)
    {
        Bins bins;
        bins.bucketCount = bucketCount;
        std::uniform_int_distribution<int> weight(0, 9);
        std::vector<int> w(GSPLAT_SORT_BINS);
        int total = 0;
        for (int& v : w) { v = weight(rng); total += v; }
        if (total == 0) { w[0] = 1; total = 1; }
        for (int i = 0; i < GSPLAT_SORT_BINS; ++i) {
            bins.divider[i] = static_cast<uint32_t>(
                static_cast<double>(w[i]) / total * bucketCount);
            bins.base[i] = i == 0 ? 0u : bins.base[i - 1] + bins.divider[i - 1];
        }
        return bins;
    }

    GSplatSortKeyParams paramsFor(const Bins& bins, float dx, float dy, float dz,
        const float minDist, const float range)
    {
        GSplatSortKeyParams p;
        p.dx = dx; p.dy = dy; p.dz = dz;
        p.minDist = minDist;
        p.invBinRange = static_cast<float>(GSPLAT_SORT_BINS) / range;
        p.binBase = bins.base.data();
        p.binDivider = bins.divider.data();
        p.bucketCount = bins.bucketCount;
        return p;
    }

    void compareBackends(const std::vector<float>& centers, const GSplatSortKeyParams& p,
        const std::string& label)
    {
        const size_t count = centers.size() / 3;
        std::vector<uint32_t> simd(count, 0xDEADBEEFu), scalar(count, 0xDEADBEEFu);
        computeGSplatSortKeys(centers.data(), count, p, simd.data());
        computeGSplatSortKeysScalar(centers.data(), count, p, scalar.data());
        size_t mismatches = 0, outOfRange = 0;
        for (size_t i = 0; i < count; ++i) {
            if (simd[i] != scalar[i]) ++mismatches;
            if (scalar[i] >= p.bucketCount) ++outOfRange;
        }
        check(mismatches == 0, label + ": " + std::to_string(mismatches) +
            " of " + std::to_string(count) + " keys differ between the SIMD and scalar paths");
        check(outOfRange == 0, label + ": " + std::to_string(outOfRange) + " keys >= bucketCount");
    }
}

int main()
{
    std::printf("gsplat sort keys (backend: %s)\n", gsplatSortKeysBackend());
    if (const char* expected = std::getenv("VISUTWIN_EXPECT_KERNEL_BACKEND");
        expected && *expected && std::string(expected) != gsplatSortKeysBackend()) {
        std::printf("FAIL expected the %s backend but this build compiled %s\n",
            expected, gsplatSortKeysBackend());
        return 1;
    }

    std::mt19937 rng(20260913);
    std::uniform_real_distribution<float> coord(-50.0f, 50.0f);

    // Every remainder around the 4-splat blocks, plus sizes large enough to matter.
    for (const size_t count : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 12u, 13u, 16u, 17u,
                               1000u, 4096u + 3u, 65537u}) {
        std::vector<float> centers(count * 3);
        for (float& v : centers) v = coord(rng);

        // Direction and depth range exactly as GSplatSorter derives them: the
        // projected distances of the bound corners.
        std::normal_distribution<float> n(0.0f, 1.0f);
        float dx = n(rng), dy = n(rng), dz = n(rng);
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        dx /= len; dy /= len; dz /= len;
        float minDist = 1e30f, maxDist = -1e30f;
        for (int c = 0; c < 8; ++c) {
            const float d = ((c & 1) ? -50.0f : 50.0f) * dx + ((c & 2) ? -50.0f : 50.0f) * dy +
                            ((c & 4) ? -50.0f : 50.0f) * dz;
            minDist = std::min(minDist, d);
            maxDist = std::max(maxDist, d);
        }
        const Bins bins = makeBins(rng, (1u << 16) + 1u);
        compareBackends(centers, paramsFor(bins, dx, dy, dz, minDist, maxDist - minDist),
            "random cloud of " + std::to_string(count));
    }

    // Depths outside the bin range: nearer than the nearest corner (the case the old
    // conversion wrapped to the farthest key on x86) and farther than the farthest.
    {
        const Bins bins = makeBins(rng, (1u << 12) + 1u);
        const GSplatSortKeyParams p = paramsFor(bins, 1.0f, 0.0f, 0.0f, 0.0f, 10.0f);
        std::vector<float> centers;
        for (const float x : {-1e-3f, -0.5f, -1000.0f, 0.0f, 10.0f, 10.001f, 11.0f, 5000.0f, 7.25f}) {
            centers.insert(centers.end(), {x, 0.0f, 0.0f});
        }
        compareBackends(centers, p, "out-of-range depths");

        std::vector<uint32_t> keys(centers.size() / 3);
        computeGSplatSortKeys(centers.data(), keys.size(), p, keys.data());
        check(keys[0] == 0u && keys[1] == 0u && keys[2] == 0u,
            "a splat nearer than the nearest bound corner must take the NEAREST key, got " +
            std::to_string(keys[0]) + ", " + std::to_string(keys[1]) + ", " + std::to_string(keys[2]));
        check(keys[7] <= p.bucketCount - 1u, "a splat far past the range must stay in range");
    }

    // Monotonic: along one axis, increasing depth must never decrease the key.
    {
        const Bins bins = makeBins(rng, (1u << 16) + 1u);
        const GSplatSortKeyParams p = paramsFor(bins, 0.0f, 0.0f, 1.0f, -20.0f, 40.0f);
        std::vector<float> centers;
        for (int i = 0; i <= 40000; ++i) {
            centers.insert(centers.end(), {0.0f, 0.0f, -20.5f + static_cast<float>(i) * 0.001025f});
        }
        std::vector<uint32_t> keys(centers.size() / 3);
        computeGSplatSortKeys(centers.data(), keys.size(), p, keys.data());
        size_t decreases = 0;
        for (size_t i = 1; i < keys.size(); ++i) {
            if (keys[i] < keys[i - 1]) ++decreases;
        }
        check(decreases == 0, std::to_string(decreases) + " places where a deeper splat got a smaller key");
    }

    if (failures != 0) {
        std::printf("gsplat sort keys: %d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("gsplat sort keys: all checks passed\n");
    return 0;
}
