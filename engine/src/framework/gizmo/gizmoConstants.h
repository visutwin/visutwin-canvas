// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Transform gizmo constants and colours.
//
#pragma once

#include <optional>

#include "core/math/color.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    /// The coordinate system the gizmo's axes follow.
    enum class GizmoSpace
    {
        World,
        Local
    };

    /// A shape's axis is one of X, Y, Z, F (the axis facing the camera)
    /// or XYZ; the plane handles are keyed YZ / XZ / XY but carry the axis of their NORMAL,
    /// so the plane shape of the YZ handle reports X.
    enum class GizmoAxis
    {
        None,
        X,
        Y,
        Z,
        YZ,
        XZ,
        XY,
        XYZ,
        F
    };

    /// How the shapes show while a drag is in progress.
    enum class GizmoDragMode
    {
        Show,       ///< always show the shapes
        Hide,       ///< hide the shapes when dragging
        Selected    ///< show only the shapes of the affected axes
    };

    /// The component index (0, 1, 2) of a single-axis value, or -1 for anything else
    /// (XYZ and F name no single component).
    constexpr int gizmoAxisIndex(const GizmoAxis axis)
    {
        switch (axis) {
            case GizmoAxis::X: return 0;
            case GizmoAxis::Y: return 1;
            case GizmoAxis::Z: return 2;
            default: return -1;
        }
    }

    /// A single-axis key's name ('x', 'y', 'z', 'yz', ...), for entity names.
    const char* gizmoAxisName(GizmoAxis axis);

    /// True for a plane handle's key (YZ, XZ, XY).
    constexpr bool gizmoAxisIsPlaneKey(const GizmoAxis axis)
    {
        return axis == GizmoAxis::YZ || axis == GizmoAxis::XZ || axis == GizmoAxis::XY;
    }

    /// Whether a plane key includes a single axis.
    constexpr bool gizmoPlaneKeyIncludes(const GizmoAxis planeKey, const GizmoAxis axis)
    {
        switch (planeKey) {
            case GizmoAxis::YZ: return axis == GizmoAxis::Y || axis == GizmoAxis::Z;
            case GizmoAxis::XZ: return axis == GizmoAxis::X || axis == GizmoAxis::Z;
            case GizmoAxis::XY: return axis == GizmoAxis::X || axis == GizmoAxis::Y;
            default: return planeKey == axis;
        }
    }

    /// `v` with component `index` replaced by `value`; any other
    /// index leaves `v` alone.
    inline Vector3 withGizmoComponent(const Vector3& v, const int index, const float value)
    {
        switch (index) {
            case 0: return Vector3(value, v.getY(), v.getZ());
            case 1: return Vector3(v.getX(), value, v.getZ());
            case 2: return Vector3(v.getX(), v.getY(), value);
            default: return v;
        }
    }

    /// Exact component-wise equality.
    inline bool gizmoVectorEquals(const Vector3& a, const Vector3& b)
    {
        return a.getX() == b.getX() && a.getY() == b.getY() && a.getZ() == b.getZ();
    }

    // color.js
    inline const Color GIZMO_COLOR_RED{1.0f, 0.3f, 0.3f, 1.0f};
    inline const Color GIZMO_COLOR_GREEN{0.3f, 1.0f, 0.3f, 1.0f};
    inline const Color GIZMO_COLOR_BLUE{0.3f, 0.3f, 1.0f, 1.0f};
    inline const Color GIZMO_COLOR_YELLOW{1.0f, 1.0f, 0.5f, 1.0f};
    inline const Color GIZMO_COLOR_GRAY{0.5f, 0.5f, 0.5f, 0.5f};

    /// The colours every shape and guide line of a transform gizmo
    /// draws with. Shapes hold POINTERS to these colours,
    /// so a theme change reaches them without rebuilding anything.
    struct GizmoTheme
    {
        struct AxisColors
        {
            Color x, y, z, xyz, f;
        };
        struct GuideColors
        {
            Color x, y, z;
        };

        AxisColors shapeBase;
        AxisColors shapeHover;
        GuideColors guideBase;
        float guideOcclusion = 0.8f;
        Color disabled;
    };

    /// `setTheme(partial)`: every field that is set is copied in.
    struct GizmoThemePartial
    {
        struct AxisColors
        {
            std::optional<Color> x, y, z, xyz, f;
        };
        struct GuideColors
        {
            std::optional<Color> x, y, z;
        };

        AxisColors shapeBase;
        AxisColors shapeHover;
        GuideColors guideBase;
        std::optional<float> guideOcclusion;
        std::optional<Color> disabled;
    };
}
