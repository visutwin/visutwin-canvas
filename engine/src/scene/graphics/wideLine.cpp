// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "wideLine.h"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    void WideLine::setPoints(const std::vector<float>& positions,
        const std::vector<float>& colors, const std::vector<float>& widths)
    {
        if (positions.size() < 6 || positions.size() % 3 != 0) {
            spdlog::error("WideLine::setPoints: positions must be packed xyz with at "
                          "least two points (got {} floats)", positions.size());
            return;
        }

        const size_t points = positions.size() / 3;
        if (colors.size() != 3 && colors.size() != points * 3) {
            spdlog::error("WideLine::setPoints: colors must be one rgb or one per point");
            return;
        }
        if (widths.size() != 1 && widths.size() != points) {
            spdlog::error("WideLine::setPoints: widths must be one value or one per point");
            return;
        }

        _positions = positions;
        _colors = colors;
        _widths = widths;
        _dirty = true;
    }

    void WideLine::setPoints(const std::vector<Vector3>& positions, const Color& color,
        const float width)
    {
        std::vector<float> packed;
        packed.reserve(positions.size() * 3);
        for (const auto& p : positions) {
            packed.push_back(p.getX());
            packed.push_back(p.getY());
            packed.push_back(p.getZ());
        }
        setPoints(packed, {color.r, color.g, color.b}, {width});
    }

    void WideLine::setDash(const float dashLength, const float gapLength, const float offset)
    {
        _dashLength = std::max(dashLength, 0.0f);
        _gapLength = std::max(gapLength, 0.0f);
        _dashOffset = offset;
        _dirty = true;
    }

    size_t WideLine::segmentCount() const
    {
        const size_t points = pointCount();
        if (points < 2) {
            return 0;
        }
        return _closed ? points : points - 1;
    }
}
