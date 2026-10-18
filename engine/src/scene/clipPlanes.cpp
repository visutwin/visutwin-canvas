// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
#include "clipPlanes.h"

#include <algorithm>
#include <atomic>

namespace visutwin::canvas
{
    namespace
    {
        bool samePlane(const Plane& a, const Plane& b)
        {
            return a.normal().getX() == b.normal().getX() && a.normal().getY() == b.normal().getY() &&
                a.normal().getZ() == b.normal().getZ() && a.distance() == b.distance();
        }
    }

    void ClipPlanes::setPlanes(const std::span<const Plane> planes)
    {
        const size_t count = std::min(planes.size(), kMaxPlanes);
        // Copy first: the span may be this set's own planes.
        std::vector<Plane> copy(planes.begin(), planes.begin() + static_cast<std::ptrdiff_t>(count));
        _planes = std::move(copy);
        changed();
    }

    void ClipPlanes::setIntersection(const bool value)
    {
        if (_intersection != value) {
            _intersection = value;
            changed();
        }
    }

    void ClipPlanes::packPlanes(float (&out)[kMaxPlanes][4]) const
    {
        for (size_t i = 0; i < kMaxPlanes; ++i) {
            if (i < _planes.size()) {
                const Plane& plane = _planes[i];
                out[i][0] = plane.normal().getX();
                out[i][1] = plane.normal().getY();
                out[i][2] = plane.normal().getZ();
                out[i][3] = plane.distance();
            } else {
                out[i][0] = out[i][1] = out[i][2] = out[i][3] = 0.0f;
            }
        }
    }

    bool ClipPlanes::operator==(const ClipPlanes& other) const
    {
        return _intersection == other._intersection &&
            std::equal(_planes.begin(), _planes.end(), other._planes.begin(), other._planes.end(), samePlane);
    }

    void ClipPlanes::changed()
    {
        static std::atomic<uint64_t> counter{0};
        _version = ++counter;
    }
}
