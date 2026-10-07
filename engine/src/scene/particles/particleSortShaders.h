// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The particle sort's uniform block and workgroup size (the kernel is
// engine/shaders/slang/programs/particle-sort.slang). Particle sorting, as one compute
// dispatch of ONE workgroup:
// each thread keys a share of the pool — sorted ascending: minus the
// squared distance to the camera (farthest drawn first), the life (newest first) or minus
// the life (oldest first) — then the workgroup runs a bitonic sort over the pool rounded
// up to a power of two (padding keys sort last), with a barrier between stages, and
// writes the particle indices in draw order. One workgroup keeps every stage's barrier a
// workgroup barrier, so the sort is a single dispatch at any pool size.
//
// Bindings follow Compute's name-order contract: buffers "order", "particles",
// "sortKeys" at 0, 1, 2 and the uniform block after them (buffer(3) / binding 3).
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::particle_sort_shaders
{
    struct alignas(16) SortUniforms
    {
        float cameraPosition[4];   // xyz the camera, in the particles' space; w the sort mode
        float counts[4];           // x particle count, y the power of two sorted over
    };
    static_assert(sizeof(SortUniforms) == 32);

    constexpr uint32_t kSortThreads = 256u;

}
