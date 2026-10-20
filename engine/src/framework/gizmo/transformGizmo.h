// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// What the translate, rotate and scale gizmos
// share — coloured X, Y and Z handles with plane and centre shapes, hover colouring by
// theme, a drag that fires `transform:start` (Vector3 point, float x, float y),
// `transform:move` (the same) and `transform:end`, snapping, the drag mode, and the
// span guide lines drawn through the gizmo along the active axes, and a readout of the
// transform delta while dragging.
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
#include "framework/gizmo/gizmo.h"
#include "framework/gizmo/shape/shape.h"
#include "scene/graphics/wideLine.h"

namespace visutwin::canvas
{
    class PlaneShape;

    /// The transform readout as a gizmo leaves it each frame, for the application to draw:
    /// DEVIATION: upstream shows it as an HTML element over the canvas; here the gizmo only keeps
    /// what to show and where, and the application draws it (the examples with a UI text element,
    /// see examples/transformGizmoExample.h).
    struct GizmoReadout
    {
        bool visible = false;
        /// The text, the axis labels in their theme colour as text markup
        /// (`[color="#rrggbb"]X[/color]`).
        std::string markup;
        /// Where it goes, in canvas points from the top-left: the horizontal centre of the text
        /// and its top edge.
        float x = 0.0f;
        float y = 0.0f;
    };

    class WideLineRenderer;

    class TransformGizmo : public Gizmo
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

        /// Whether snapping is enabled.
        bool snap = false;
        /// The snapping increment (world units, degrees for rotation).
        float snapIncrement = 1.0f;
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
        /// The occluded guide lines draw on the gizmo layer and move with it.
        void setLayer(const std::shared_ptr<Layer>& layer) override;
        void destroy() override;

    protected:
        /// Where the pointer ray meets the drag plane.
        virtual Vector3 screenToPoint(float x, float y);

        Ray createRay(const Vector3& mouseWPos) const;
        Plane createPlane(GizmoAxis axis, bool isFacing, bool isLine) const;
        Vector3 dirFromAxis(GizmoAxis axis) const;
        static Vector3 projectToAxis(const Vector3& point, GizmoAxis axis);

        virtual void drawGuideLines(const Vector3& pos, const Quaternion& rot, GizmoAxis activeAxis,
                                    bool activeIsPlane);
        void drawSpanLine(const Vector3& pos, const Quaternion& rot, GizmoAxis axis);

        /// The axis-and-plane gizmos' (translate, scale) camera facing: hides an axis
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

    private:
        Shape* shapeOf(const MeshInstance* meshInstance) const;
        GizmoAxis axisOf(const MeshInstance* meshInstance) const;
        bool isPlaneOf(const MeshInstance* meshInstance) const;
        void hover(const MeshInstance* meshInstance);
        void flushGuideLines();
        void releaseGuideLines();

        std::vector<GizmoAxis> _hovering;

        bool _readoutEnabled = true;
        GizmoReadout _readout;

        // Span guide lines. DEVIATION: upstream draws them with app.drawLine (one-pixel
        // immediate lines) — the base colour depth-tested on the Immediate layer and the
        // occluded colour untested on the gizmo layer. Here two WideLineRenderers draw the
        // same two passes as one-pixel wide lines, clipped to the camera's near plane on
        // the CPU (a wide line expands in screen space and has no near clip of its own).
        struct GuideLine
        {
            Vector3 from;
            Vector3 to;
            Color color;
        };
        std::vector<GuideLine> _baseLines;
        std::vector<GuideLine> _occludedLines;
        std::vector<std::pair<Vector3, Vector3>> _frameGuideLines;
        std::unique_ptr<WideLineRenderer> _baseRenderer;
        std::unique_ptr<WideLineRenderer> _occludedRenderer;
        std::array<WideLine, 3> _baseWideLines;
        std::array<WideLine, 3> _occludedWideLines;
        size_t _baseShown = 0;
        size_t _occludedShown = 0;
    };
}
