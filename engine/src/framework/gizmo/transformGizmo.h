// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// What the translate, rotate and scale gizmos
// share — coloured X, Y and Z handles with plane and centre shapes, hover colouring by
// theme, a drag that fires `transform:start` (Vector3 point, float x, float y),
// `transform:move` (the same) and `transform:end`, snapping (DragGizmo), the drag mode, the
// span guide lines drawn through the gizmo along the active axes, shapes that fade out
// before they hide at glancing angles, and a readout of the transform delta while dragging.
//
// The subclasses decide what a drag does to the attached nodes.
//
#pragma once

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/shape/plane.h"
#include "core/shape/ray.h"
#include "framework/gizmo/dragGizmo.h"
#include "framework/gizmo/gizmoReadout.h"
#include "framework/gizmo/shape/shape.h"

namespace visutwin::canvas
{
    class PlaneShape;

    class OverlayRenderer;

    class TransformGizmo : public DragGizmo
    {
    public:
        static constexpr const char* EVENT_TRANSFORMSTART = "transform:start";
        static constexpr const char* EVENT_TRANSFORMMOVE = "transform:move";
        static constexpr const char* EVENT_TRANSFORMEND = "transform:end";
        /// Fired when the transformation starts and on each step of it, whether or not the
        /// readout is shown, with the selected axis, whether it is a plane, and the value the
        /// readout shows: the translation (Vector3) for a translate gizmo, the scale factor
        /// (Vector3) for a scale gizmo, the angle in degrees (float) for a rotate gizmo.
        static constexpr const char* EVENT_TRANSFORMREADOUT = "transform:readout";

        TransformGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer,
                       const std::string& name = "gizmo:transform");
        ~TransformGizmo() override;

        /// How the shapes show while dragging.
        GizmoDragMode dragMode = GizmoDragMode::Selected;

        const GizmoTheme& theme() const { return _theme; }
        void setTheme(const GizmoThemePartial& partial);

        /// Enables or disables a shape, keyed as the shapes are ('x', 'yz', 'xyz',
        /// 'f' ...).
        void enableShape(GizmoAxis shapeAxis, bool enabled);
        bool isShapeEnabled(GizmoAxis shapeAxis) const;

        /// The shape under a key, or null.
        Shape* shape(GizmoAxis key) const;

        /// The thickness of the guide lines, in points (1 by default).
        float guideThickness() const;
        void setGuideThickness(float value);

        /// Whether the transform delta is shown in a readout just below the gizmo while dragging.
        /// On by default. Off, `transform:readout` still fires, for a UI of the application's own.
        bool readout() const { return _readoutEnabled; }
        void setReadout(bool value);
        /// What the readout shows this frame, and where.
        const GizmoReadout& readoutState() const { return _readout; }

        GizmoAxis hoverAxis() const { return _hoverAxis; }
        GizmoAxis selectedAxis() const { return _selectedAxis; }
        bool selectedIsPlane() const { return _selectedIsPlane; }
        bool dragging() const { return _hoverAxis == GizmoAxis::None && _selectedAxis != GizmoAxis::None; }

        /// The guide lines drawn this frame, as world-space (from, to) pairs before the
        /// near-plane clip; for tests and debugging.
        const std::vector<std::pair<Vector3, Vector3>>& guideLines() const { return _frameGuideLines; }

        void prerender() override;
        void destroy() override;

    protected:
        /// Where the pointer ray meets the drag plane.
        virtual Vector3 screenToPoint(float x, float y);

        Plane createPlane(GizmoAxis axis, bool isFacing, bool isLine) const;
        Vector3 dirFromAxis(GizmoAxis axis) const;
        static Vector3 projectToAxis(const Vector3& point, GizmoAxis axis);

        virtual void drawGuideLines(const Vector3& pos, const Quaternion& rot, GizmoAxis activeAxis,
                                    bool activeIsPlane);
        void drawSpanLine(const Vector3& pos, const Quaternion& rot, GizmoAxis axis);

        /// Hides a shape when the view looks along its direction, fading it out over the fade
        /// angle before. `cos` is the absolute cosine of the angle between the view and the
        /// direction. True when its visibility changed.
        bool glance(Shape* shape, float cos);

        /// The axis-and-plane gizmos' (translate, scale) camera facing: fades and hides an axis
        /// seen end-on and a plane seen edge-on, and with `flipPlanes` turns each plane
        /// toward the camera. `axes` and `planes` are x/y/z and yz/xz/xy.
        void updateAxisAndPlaneShapes(const std::array<Shape*, 3>& axes, const std::array<PlaneShape*, 3>& planes,
            bool flipPlanes);
        /// The same gizmos' shape visibility while dragging, by dragMode.
        void applyAxisDragVisibility(bool state);

        /// Parents every shape under the root and makes it
        /// pickable. Shapes are added in their key order, which is their draw order.
        void createTransform();

        template <typename T, typename... Args>
        T* addShape(GizmoAxis key, Args&&... args)
        {
            auto shape = std::make_unique<T>(std::forward<Args>(args)...);
            T* raw = shape.get();
            _shapes.emplace_back(key, std::move(shape));
            return raw;
        }

        /// An axis letter in the axis colour, as text markup.
        std::string readoutLabel(GizmoAxis axis) const;
        /// A delta with an explicit sign, so it does not read as an absolute value; a value that
        /// rounds to zero is shown as positive.
        static std::string readoutDelta(float value, int digits);
        /// A value for each axis being dragged: deltas, or with `factor` scale factors (x1.25).
        std::string readoutAxes(const Vector3& values, bool factor = false) const;
        /// Shows the readout centred below the visible shapes (while dragging, the dragged
        /// handle), or below the gizmo centre when every shape is hidden.
        void showReadout(const std::string& markup);
        /// Shows it centred on `x`, just below `y` (canvas points).
        void showReadoutAt(const std::string& markup, float x, float y);
        void hideReadout() { _readout.visible = false; }

        /// Shape arguments for an axis, coloured from the theme.
        ShapeArgs shapeArgs(GizmoAxis axis, const Vector3& rotation = Vector3(0.0f)) const;

        GizmoTheme _theme;
        Vector3 _rootStartPos = Vector3(0.0f);
        Quaternion _rootStartRot;
        std::vector<std::pair<GizmoAxis, std::unique_ptr<Shape>>> _shapes;

        GizmoAxis _hoverAxis = GizmoAxis::None;
        bool _hoverIsPlane = false;
        GizmoAxis _selectedAxis = GizmoAxis::None;
        bool _selectedIsPlane = false;
        Vector3 _selectionStartPoint = Vector3(0.0f);

        /// The last drag-plane hit: a plane the ray misses leaves the previous value.
        Vector3 _point = Vector3(0.0f);

        /// The angle in degrees between the view and a shape below which it fades out.
        float _fadeAngle = 25.0f;

    private:
        Shape* shapeOf(const MeshInstance* meshInstance) const;
        GizmoAxis axisOf(const MeshInstance* meshInstance) const;
        bool isPlaneOf(const MeshInstance* meshInstance) const;
        void hover(const MeshInstance* meshInstance);

        std::vector<GizmoAxis> _hovering;

        bool _readoutEnabled = true;
        GizmoReadout _readout;

        // The guide lines, on an unrotated root in the Immediate layer, whose depth lets the
        // scene hide them. The guide colours' rgb each guide replaces with its axis colour.
        Color _guideBase = Color(1.0f, 1.0f, 1.0f, 1.0f);
        Color _guideOccluded = Color(1.0f, 1.0f, 1.0f, 1.0f);
        Color _guideFill = Color(1.0f, 1.0f, 1.0f, 0.0f);
        Entity* _guideRoot = nullptr;
        std::unique_ptr<OverlayRenderer> _guides;
        std::vector<std::pair<Vector3, Vector3>> _frameGuideLines;
    };
}
