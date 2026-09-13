// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <cstddef>
#include <cstdint>

namespace visutwin::canvas
{
    /// Number of density bins GSplatSorter distributes its sort-key bits over.
    inline constexpr int GSPLAT_SORT_BINS = 32;

    /**
     * The per-splat half of the gaussian splat depth sort: project each centre onto
     * the sort direction and map that depth to a counting-sort key, using the
     * density-shaped bins GSplatSorter derives from its chunk histogram.
     */
    struct GSplatSortKeyParams
    {
        float dx = 0.0f, dy = 0.0f, dz = 0.0f;  ///< sort direction
        float minDist = 0.0f;                   ///< projected distance of the nearest bound corner
        float invBinRange = 0.0f;               ///< GSPLAT_SORT_BINS / (maxDist - minDist)
        const uint32_t* binBase = nullptr;      ///< first key of each bin
        const uint32_t* binDivider = nullptr;   ///< number of keys in each bin
        uint32_t bucketCount = 1;               ///< keys are in [0, bucketCount)
    };

    /**
     * Writes one key per splat. `centers` is xyz per splat.
     *
     * Uses 4-lane SIMD where the target has it (SSE2 on x86, NEON on AArch64) and
     * the scalar reference otherwise. The two are REQUIRED to agree bit for bit,
     * which is why the file implementing them is compiled with -ffp-contract=off:
     * GCC and clang both fuse `a * b + c` into FMA by default — GCC even in strict
     * C++ mode once -mfma is on, which Jolt's exported flags turn on — and a fused
     * scalar path rounds differently from the SIMD one.
     */
    void computeGSplatSortKeys(const float* centers, size_t count,
        const GSplatSortKeyParams& params, uint32_t* outKeys);

    /// The scalar reference `computeGSplatSortKeys` must match. Public for its test.
    void computeGSplatSortKeysScalar(const float* centers, size_t count,
        const GSplatSortKeyParams& params, uint32_t* outKeys);

    /// Which implementation `computeGSplatSortKeys` compiled to: "sse2", "neon" or "scalar".
    const char* gsplatSortKeysBackend();
}
