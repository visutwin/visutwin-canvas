// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A connected, variable-width polyline (upstream extras/renderers/wide-line.js).
#pragma once

#include <cstdint>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    /// Butt caps stop exactly at the first and last points; square extends past them
    /// by half the width; round draws a half disc.
    enum class LineCap : uint32_t { Butt = 0, Square = 1, Round = 2 };

    /// Miter extends the edges until they meet, bevel connects them directly, round
    /// fills the corner with a disc.
    enum class LineJoin : uint32_t { Miter = 0, Bevel = 1, Round = 2 };

    /**
     * One polyline. Point data is held as packed arrays rather than a vector of
     * structs, so a line of thousands of points costs one allocation per channel.
     *
     * A line belongs to at most one WideLineRenderer. Removing it leaves the data
     * intact so it can be handed to another renderer.
     */
    class WideLine
    {
    public:
        /**
         * Replaces all point data at once — the only call that can change the point
         * count. Colours and widths may be given per point or as a single value
         * used by every point.
         *
         * @param positions Packed xyz, so `size()` is three times the point count,
         *                  and at least two points.
         * @param colors    Packed rgb, either one per point or exactly one colour.
         * @param widths    One per point, or exactly one width.
         */
        void setPoints(const std::vector<float>& positions,
                       const std::vector<float>& colors = {1.0f, 1.0f, 1.0f},
                       const std::vector<float>& widths = {1.0f});

        /// Convenience form: a uniform colour and width.
        void setPoints(const std::vector<Vector3>& positions, const Color& color, float width);

        [[nodiscard]] size_t pointCount() const { return _positions.size() / 3; }
        [[nodiscard]] const std::vector<float>& positions() const { return _positions; }
        [[nodiscard]] const std::vector<float>& colors() const { return _colors; }
        [[nodiscard]] const std::vector<float>& widths() const { return _widths; }

        LineCap cap() const { return _cap; }
        void setCap(const LineCap value) { _cap = value; _dirty = true; }
        LineJoin join() const { return _join; }
        void setJoin(const LineJoin value) { _join = value; _dirty = true; }

        /// A closed line joins its last point back to its first, and has no caps.
        bool closed() const { return _closed; }
        void setClosed(const bool value) { _closed = value; _dirty = true; }

        /// Dash and gap lengths in the same units as the line's own distance along
        /// itself (world units). Both zero draws a solid line.
        void setDash(float dashLength, float gapLength, float offset = 0.0f);
        float dashLength() const { return _dashLength; }
        float gapLength() const { return _gapLength; }
        float dashOffset() const { return _dashOffset; }

        /// Segment count: one per pair of adjacent points, plus the closing one.
        [[nodiscard]] size_t segmentCount() const;

        [[nodiscard]] bool dirty() const { return _dirty; }
        void clearDirty() { _dirty = false; }

    private:
        std::vector<float> _positions;
        std::vector<float> _colors{1.0f, 1.0f, 1.0f};
        std::vector<float> _widths{1.0f};

        LineCap _cap = LineCap::Butt;
        LineJoin _join = LineJoin::Miter;
        bool _closed = false;
        float _dashLength = 0.0f;
        float _gapLength = 0.0f;
        float _dashOffset = 0.0f;
        bool _dirty = true;
    };
}
