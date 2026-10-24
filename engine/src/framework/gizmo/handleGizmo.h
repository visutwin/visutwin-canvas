// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// The base of the gizmos that draw world-sized overlay shapes, such as a camera frustum or a
// light shape, with handles that drag to edit them once large enough on screen.
//
// Use a layer that keeps the scene depth, so occluded parts take the `occluded` colour, and sorts
// transparent mesh instances manually, as Gizmo::createLayer makes with `depth` set:
//
//     auto layer = Gizmo::createLayer(engine, "Overlays", INT_MAX, true);
//
// Events: `handle:start`, `handle:move`, `handle:end` (GraphNode* — null for a shape gizmo —
// and const std::string& naming the value the handle edits) and `icon:select` (GraphNode*),
// fired when a node's icon is clicked.
//
// The shapes are lines and rings of a constant screen thickness, handles of a constant screen
// size and icons (OverlayRenderer), placed again every frame; a hovered or dragged handle shows
// its value below it in `labels()`, for the application to draw (see gizmoReadout.h).
//
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/math/color.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "framework/gizmo/dragGizmo.h"
#include "framework/gizmo/gizmoReadout.h"
#include "framework/gizmo/overlay/iconGlyphs.h"
#include "framework/gizmo/pool.h"

namespace visutwin::canvas
{
    class OverlayRenderer;
    class Shape;

    struct HandleGizmoTheme
    {
        /// Where the shapes are visible.
        Color base;
        /// Where the scene hides the shapes.
        Color occluded;
        /// Filled volumes; an alpha of 0 skips them.
        Color fill;
        /// A hovered or dragged radius, range, width or far clip handle, its guide and label.
        Color primary;
        /// A hovered or dragged height, near clip or inner cone angle handle.
        Color secondary;
        /// A hovered or dragged depth, field of view or outer cone angle handle.
        Color tertiary;
        /// A hovered or dragged box corner handle.
        Color corner;
        /// The disc behind each icon, which is clicked to select it.
        Color backdrop;
    };

    struct HandleGizmoThemePartial
    {
        std::optional<Color> base, occluded, fill, primary, secondary, tertiary, corner, backdrop;
    };

    /// A handle placed this frame, or the one being dragged.
    struct GizmoHandle
    {
        Shape* shape = nullptr;
        /// The node the handle edits, or null for a shape gizmo.
        GraphNode* node = nullptr;
        /// The value the handle edits ("" for none).
        std::string name;
        /// The handle's index among those editing the value.
        int index = 0;
        /// The point the edit is measured from.
        Vector3 origin = Vector3(0.0f);
        /// The unit direction of the line, or the normal of the plane.
        Vector3 dir = Vector3(0.0f);
        /// Whether the handle moves in a plane rather than along a line.
        bool plane = false;
    };

    /// The handles of a box and how they resize it.
    struct BoxHandleOptions
    {
        /// Handles at the centre of each face.
        bool faceHandles = true;
        /// Handles at the corners.
        bool cornerHandles = true;
        /// A resize keeps the centre in place.
        bool centered = false;
        /// A corner handle scales the box evenly.
        bool uniform = false;
    };

    class HandleGizmo : public DragGizmo
    {
    public:
        static constexpr const char* EVENT_HANDLESTART = "handle:start";
        static constexpr const char* EVENT_HANDLEMOVE = "handle:move";
        static constexpr const char* EVENT_HANDLEEND = "handle:end";
        static constexpr const char* EVENT_ICONSELECT = "icon:select";

        HandleGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer, const std::string& name = "gizmo:handle");
        ~HandleGizmo() override;

        /// Whether to show the handles that edit the shapes.
        bool handles = true;
        /// Whether to show the value of a hovered or dragged handle.
        bool readout = true;
        /// Whether to show an icon at each node.
        bool icon = true;

        /// The line thickness in points (2); dashed lines space their dashes to match.
        float lineThickness() const;
        void setLineThickness(float value);
        /// The handle size in points (7).
        float handleSize() const;
        void setHandleSize(float value);
        /// The icon size in points (24).
        float iconSize() const;
        void setIconSize(float value);

        const HandleGizmoTheme& theme() const { return _theme; }
        void setTheme(const HandleGizmoThemePartial& partial);

        /// The labels of this frame: the value of the hovered or dragged handle.
        const std::vector<GizmoReadout>& labels() const;

        bool dragging() const { return _dragging; }
        /// The handles placed this frame; for tests.
        size_t handleCount() const { return _placed.count; }
        const GizmoHandle& placedHandle(size_t i) const { return *_placed.items()[i]; }
        const OverlayRenderer& renderer() const { return *_renderer; }

        void prerender() override;
        void destroy() override;

    protected:
        /// The shapes are drawn in world space, at world size.
        void updateRotation() override {}
        void updateScale() override {}

        /// Whether a shape is large enough on screen to show its handles, which fade in over a
        /// range of sizes rather than pop in.
        bool detailed(const Vector3& center, float radius);

        /// Places a handle that drags along a line or in a plane, with a dashed guide from the
        /// origin while hovered or dragged (on a line, unless `guide` says otherwise). `color` is
        /// the handle's and its guide's while hovered or dragged. True when it is hovered or
        /// dragged.
        bool handle(const Vector3& position, const Color& color, GraphNode* node, const std::string& name,
            int index, const Vector3& origin, const Vector3& dir, bool inPlane = false,
            std::optional<bool> guide = std::nullopt);
        /// Shows the value of a hovered or dragged handle below it.
        void readoutAt(const Vector3& position, const std::string& label, const Color& color, float value,
            const std::string& unit = "");
        /// Shows readout markup below a hovered or dragged handle.
        void readoutMarkupAt(const Vector3& position, const std::string& markup);
        /// `value` with two decimals after `label` in `color`.
        std::string readoutValue(const std::string& label, const Color& color, float value,
            const std::string& unit = "") const;
        /// Whether the handle editing a value is hovered or dragged.
        bool active(const GraphNode* node, const std::string& name, int index) const;

        /// Three great circles of a sphere.
        void sphereRings(const Vector3& center, const Quaternion& rotation, float radius, float weight = 1.0f,
            float alpha = 1.0f);
        /// The outline of a sphere, over faint great circles once large enough on screen, or solid
        /// rings when the camera is inside it.
        void sphereOutline(const Vector3& center, const Quaternion& rotation, float radius);
        /// The outline of a sphere as the camera sees it: its centre into `out`, its radius.
        float sphereSilhouette(const Vector3& center, float radius, Vector3& out) const;
        /// Handles at the ends of a sphere's axes, which drag its radius.
        void sphereHandles(const Vector3& center, const Quaternion& rotation, float radius, GraphNode* node,
            const std::string& name);

        /// The corners of a box, indexed by their x, y and z bits; valid until the next call.
        const std::array<Vector3, 8>& boxCorners(const Vector3& center, const Quaternion& rotation,
            const Vector3& halfExtents);
        /// The face and corner handles of the box last passed to boxCorners, once large enough on
        /// screen; `pad` on each side is left out of the readout extents.
        void boxHandles(const Vector3& center, const Vector3& halfExtents, const BoxHandleOptions& options,
            float pad = 0.0f);
        /// Resizes a box to a dragged boxHandles handle, keeping the opposite face or corner in
        /// place, or the centre for a centred box.
        void resizeBox(const GizmoHandle& handle, const Vector3& point, const Quaternion& rotation,
            const Vector3& startPosition, const Vector3& startHalfExtents, const BoxHandleOptions& options,
            float pad, Vector3& position, Vector3& halfExtents) const;

        /// The icon of a node, which fires `icon:select` when clicked.
        void placeIcon(OverlayIcon kind, const Vector3& position, GraphNode* node);

        /// Places the shapes of the attached nodes.
        virtual void drawShapes() {}
        /// Stores the values a handle edits at the start of a drag.
        virtual void startHandle(const GizmoHandle& /*handle*/) {}
        /// Edits the value of the dragged handle; `point` is where it is dragged to, on its line.
        virtual void moveHandle(const GizmoHandle& /*handle*/, const Vector3& /*point*/) {}

        /// The camera's world position and axes.
        Vector3 viewPosition() const { return cameraPosition(); }
        Vector3 viewForward() const { return cameraForward(); }
        bool perspective() const;

        HandleGizmoTheme _theme;
        /// The theme colours of the box faces along each axis.
        std::array<const Color*, 3> _faceColors{};
        std::unique_ptr<OverlayRenderer> _renderer;
        GizmoHandle _drag;
        bool _dragging = false;

    private:
        static bool matches(const GizmoHandle& a, const GraphNode* node, const std::string& name, int index);
        GizmoHandle* find(const MeshInstance* meshInstance) const;
        std::optional<Vector3> pointerPoint(float x, float y) const;
        void endDrag();

        Pool<GizmoHandle> _placed{[]() { return std::make_unique<GizmoHandle>(); }};
        GizmoHandle _hovered;
        float _detailFade = 1.0f;
        Vector3 _dragOffset = Vector3(0.0f);
        std::array<Vector3, 8> _boxCorners{};
        std::array<Vector3, 3> _boxAxes{};
    };

    /// The world axes of a node: its world transform's x, y and -z, normalised.
    Vector3 gizmoNodeRight(GraphNode* node);
    Vector3 gizmoNodeUp(GraphNode* node);
    Vector3 gizmoNodeForward(GraphNode* node);
}
