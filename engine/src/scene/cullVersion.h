// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
// A process-wide counter moved by every change the mesh-instance cull sweep or the
// forward draw sort reads: a node carrying mesh instances moving (GraphNode marks the
// nodes that do, so a camera or a light moving does not count), a mesh instance's
// material, mask, visibility, cull flag, bounds, bucket or draw order, a mesh's bounds or
// vertex buffer, a material's transparency, skybox or alpha mode, a render component's
// layers, instances or active state, a layer's own instances, and a component joining or
// leaving a registry. The renderer stamps each (camera, layer) cull set with the value
// it was culled at and keeps the set, and its sorted draw order, for as long as the
// value, the camera's frustum, culling mask and sky flag stay the same: a static scene
// then skips the sweep and the sort every frame.
//
// A writer that changes something the sweep reads and does not bump leaves a camera
// drawing last frame's set for good, so a new input to the sweep is a new bump.

#pragma once

#include <atomic>
#include <cstdint>

namespace visutwin::canvas
{
    class CullVersion
    {
    public:
        /// The current value. Never 0, so a set stamped 0 never matches.
        static uint64_t current() noexcept { return _counter.load(std::memory_order_relaxed); }

        /// Moves the value on. Cheap enough for a node's dirty path.
        static void bump() noexcept { _counter.fetch_add(1, std::memory_order_relaxed); }

    private:
        static inline std::atomic<uint64_t> _counter{1};
    };
}
