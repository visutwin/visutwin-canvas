// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
// The clipping of a mesh instance, a camera or a render component: up to eight world-space
// planes and whether they clip by intersection. A point is behind a plane when
// dot(normal, p) + distance < 0; a fragment behind ANY plane of a set is discarded, or with
// intersection clipping only one behind ALL of them, which cuts a wedge or a corner out as a
// cutaway does. A set with no planes clips nothing. The planes of a mesh instance and of its
// camera are two sets, each tested on its own.
//
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/shape/plane.h"

namespace visutwin::canvas
{
    /// The sets of clipping planes a draw is clipped by, as a shader variant input.
    enum ClipPlaneSets : uint32_t
    {
        CLIP_PLANES_NONE = 0,
        CLIP_PLANES_MESH = 1,     // the mesh instance's own planes (every pass, shadows included)
        CLIP_PLANES_CAMERA = 2,   // the camera's planes (the camera's passes, not shadows)
    };

    class ClipPlanes
    {
    public:
        /// The largest number of planes a set holds; further planes are ignored.
        static constexpr size_t kMaxPlanes = 8;

        /// Copies the planes, at most kMaxPlanes of them.
        void setPlanes(std::span<const Plane> planes);
        const std::vector<Plane>& planes() const { return _planes; }
        bool empty() const { return _planes.empty(); }

        /// Whether only what is behind ALL of the planes is clipped, instead of what is behind
        /// any of them.
        void setIntersection(bool value);
        bool intersection() const { return _intersection; }

        /// Names the planes and mode as they are now: changes whenever either does, unique
        /// across every set in the process. 0 for a set that never had planes.
        uint64_t version() const { return _version; }

        /// The planes as the shader reads them, (normal, distance) per plane, unused ones zero.
        void packPlanes(float (&out)[kMaxPlanes][4]) const;

        bool operator==(const ClipPlanes& other) const;

    private:
        void changed();

        std::vector<Plane> _planes;
        bool _intersection = false;
        uint64_t _version = 0;
    };
}
