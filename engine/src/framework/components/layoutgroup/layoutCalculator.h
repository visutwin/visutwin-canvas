// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// The layout a layout group gives its children, as a pure function: the caller reads each
// child's sizes, limits and pivot, and applies the sizes and positions that come back. The
// arithmetic, its order and its double precision follow upstream exactly.
//
#pragma once

#include <limits>
#include <vector>

#include "core/math/vector2.h"
#include "core/math/vector4.h"

namespace visutwin::canvas
{
    /// Whether a layout group lays its children out in a row or a column.
    enum class Orientation
    {
        Horizontal = 0,
        Vertical = 1
    };

    /// How a layout group fits its children to its size along an axis.
    enum class LayoutFitting
    {
        /// Children keep their sizes.
        None = 0,
        /// Children grow, up to their maximum sizes, to fill the group when they are smaller.
        Stretch = 1,
        /// Children shrink, down to their minimum sizes, to fit the group when they are larger.
        Shrink = 2,
        /// Both.
        Both = 3
    };

    struct LayoutOptions
    {
        Orientation orientation = Orientation::Horizontal;
        bool reverseX = false;
        bool reverseY = true;
        /// Where the laid-out block sits in the group: 0 left or bottom, 1 right or top.
        Vector2 alignment = Vector2(0.0f, 1.0f);
        /// Left, bottom, right, top.
        Vector4 padding = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        Vector2 spacing = Vector2(0.0f, 0.0f);
        LayoutFitting widthFitting = LayoutFitting::None;
        LayoutFitting heightFitting = LayoutFitting::None;
        bool wrap = false;
        Vector2 containerSize = Vector2(0.0f, 0.0f);
    };

    /// One child: its own size and pivot, and the limits and share its layout child gives it
    /// (the defaults where it has none).
    struct LayoutItem
    {
        float width = 0.0f;
        float height = 0.0f;
        Vector2 pivot = Vector2(0.0f, 0.0f);
        float minWidth = 0.0f;
        float minHeight = 0.0f;
        float maxWidth = std::numeric_limits<float>::infinity();
        float maxHeight = std::numeric_limits<float>::infinity();
        float fitWidthProportion = 0.0f;
        float fitHeightProportion = 0.0f;
    };

    /// Where a child goes: its calculated size and its local position.
    struct LayoutPlacement
    {
        float width = 0.0f;
        float height = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
    };

    struct LayoutResult
    {
        /// One per item, in the items' order.
        std::vector<LayoutPlacement> placements;
        /// x and y of the laid-out block's bottom-left corner
        /// within the group, its width and its height.
        Vector4 bounds = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    };

    /// Lay out `items` (the children taking part, in order) in a group of `options`.
    LayoutResult calculateLayout(const std::vector<LayoutItem>& items, const LayoutOptions& options);
}
