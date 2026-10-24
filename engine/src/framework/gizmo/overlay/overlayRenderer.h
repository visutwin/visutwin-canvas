// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Pooled shapes a gizmo places each frame between begin() and end(): lines and rings of a
// constant screen thickness, handles of a constant screen size, filled volumes, labels and
// icons, in world space under a parent that must not be rotated or scaled. The shapes follow a
// theme whose colours the owner keeps: `base` where they are visible, `occluded` where the scene
// hides them (drawn first, without depth test), `fill` for the volumes and `backdrop` behind the
// icons. In a manually sorted layer the volumes draw first, then everything else in placement
// order, so a line drawn after a shape draws over it.
//
// Sizes in "points" are canvas points, upstream's CSS pixels.
//
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/math/color.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "framework/gizmo/gizmoReadout.h"
#include "framework/gizmo/meshLineBatch.h"
#include "framework/gizmo/overlay/iconGlyphs.h"
#include "framework/gizmo/overlay/overlayIcons.h"
#include "framework/gizmo/overlay/shapePool.h"

namespace visutwin::canvas
{
    class ArcShape;
    class BoxShape;
    class Entity;
    class Gizmo;
    class Layer;
    class Shape;

    /// The colours an overlay renderer follows, kept by its owner. `fill` is needed only to place
    /// volumes and `backdrop` only to place icons.
    struct OverlayRendererTheme
    {
        Color* base = nullptr;
        Color* occluded = nullptr;
        Color* fill = nullptr;
        Color* backdrop = nullptr;
    };

    /// The kinds of filled cone: a cone, a flat disc, a cylinder, a flat triangle and a square
    /// frustum.
    enum class OverlayCone
    {
        Cone,
        Disk,
        Cylinder,
        Triangle,
        Frustum,
        Count
    };

    class OverlayRenderer
    {
    public:
        /// Shapes of `gizmo` under `parent` (the gizmo's root by default), on `layer` (which must
        /// keep the scene depth), or on the gizmo's layer, following it, when null.
        OverlayRenderer(Gizmo* gizmo, const OverlayRendererTheme& theme, Entity* parent = nullptr,
            std::shared_ptr<Layer> layer = nullptr);
        ~OverlayRenderer();

        OverlayRenderer(const OverlayRenderer&) = delete;
        OverlayRenderer& operator=(const OverlayRenderer&) = delete;

        /// The line thickness in points.
        float lineThickness = 2.0f;
        /// The handle size in points.
        float handleSize = 7.0f;
        /// The icon size in points.
        float iconSize = 24.0f;
        /// An rgb replacing the theme's, or null to keep it.
        const Color* tint = nullptr;

        const OverlayRendererTheme& theme() const { return _theme; }
        OverlayIcons& icons() { return *_icons; }
        const OverlayIcons& icons() const { return *_icons; }

        /// Starts a frame.
        void begin();
        /// Hides the shapes not placed this frame.
        void end();
        /// Hides every shape and label.
        void clear();

        /// The radius on screen, in points, of a sphere.
        float screenSize(const Vector3& center, float radius) const;
        /// The world size of `count` points at a world point.
        float pixels(const Vector3& point, float count) const;

        /// A line between two world points, `weight` times the line thickness, its alpha scaled
        /// by `alpha`, `color` replacing the theme's rgb when given.
        void line(const Vector3& a, const Vector3& b, float weight = 1.0f, float alpha = 1.0f,
            const Color* color = nullptr);
        /// A dashed line, its dashes evenly spaced on screen.
        void dashed(const Vector3& a, const Vector3& b, float weight = 1.0f, float alpha = 1.0f,
            const Color* color = nullptr);
        /// An arc from angle `from` to `to` (radians), x the unit direction at angle 0 and y the
        /// one a quarter turn on.
        void arc(const Vector3& center, const Vector3& x, const Vector3& y, float radius, float from, float to,
            float weight = 1.0f, float alpha = 1.0f, const Color* color = nullptr);
        /// A ring in the xz plane of the rotation.
        void ring(const Vector3& center, const Quaternion& rotation, float radius, float weight = 1.0f,
            float alpha = 1.0f);
        /// A ring facing `normal` (need not be normalised).
        void circle(const Vector3& center, const Vector3& normal, float radius, float weight = 1.0f,
            float alpha = 1.0f);
        /// A dashed ring facing `normal`.
        void dashedCircle(const Vector3& center, const Vector3& normal, float radius, float weight = 1.0f,
            float alpha = 1.0f);
        /// An arrow along the y axis of the rotation, its base at `position`, scaled as a
        /// transform gizmo is.
        void arrow(const Vector3& position, const Quaternion& rotation, float scale);
        /// A pickable handle, a cube of a constant screen size, in `hover`'s rgb when given.
        BoxShape* handle(const Vector3& position, const Color* hover = nullptr);
        /// A corner bracket arm along world axis `axis` (0 x, 1 y, 2 z), on side `sign` of the
        /// corner, `length` from it, `thickness` points thick, overhanging so the arms meet.
        void arm(const Vector3& corner, int axis, float sign, float length, float thickness);
        /// A dot of a constant screen size, `radius` points.
        void dot(const Vector3& position, float radius);
        /// A filled box, `size` along each axis of the rotation, in the fill colour unless
        /// `volume` is false.
        Shape* box(const Vector3& center, const Quaternion& rotation, const Vector3& size, float alpha = 1.0f,
            bool volume = true);
        /// A filled sphere.
        void sphere(const Vector3& center, float radius, float alpha = 1.0f);
        /// A filled capsule along the y axis of the rotation, `height` caps included.
        void capsule(const Vector3& center, const Quaternion& rotation, float radius, float height,
            float alpha = 1.0f);
        /// A filled unit cone along the y axis of the rotation, its base at y = -0.5; `size` 1
        /// gives a base radius of 0.5. A disk and a triangle are flattened by the size, a cylinder
        /// keeps its radius to the peak, a frustum is a square pyramid whose peak radius is `peak`
        /// of its base's.
        void cone(OverlayCone kind, const Vector3& center, const Quaternion& rotation, const Vector3& size,
            float alpha = 1.0f, bool volume = true, float peak = 0.0f);
        /// A flat disc facing the y axis of the rotation, picked by the gizmo.
        Shape* disc(const Vector3& center, const Quaternion& rotation, float size, float alpha = 1.0f,
            const Color* color = nullptr);
        /// An icon glyph on the xz plane of the rotation, its top toward -z.
        Shape* glyph(OverlayIcon kind, const Vector3& center, const Quaternion& rotation, float size);

        /// A text label just below a world point, `below` points further down.
        void label(const Vector3& position, const std::string& markup, float below = 0.0f);
        /// The labels of this frame.
        const std::vector<GizmoReadout>& labels() const { return _labels; }

        /// Frees the shapes. Safe to call again.
        void destroy();

        /// The line batch; for tests.
        const MeshLineBatch& lineBatch() const { return *_lines; }

    private:
        ShapePool& pool(const ShapePool::Create& create, uint32_t drawOrder);
        void moveTo(const std::shared_ptr<Layer>& layer);
        float viewDepth(const Vector3& point) const;
        float depth(const Vector3& point) const;
        Vector3 local(const Vector3& point) const;
        void colors(float alpha, bool volume = false, const Color* color = nullptr);
        Shape* next(ShapePool& pool);
        void piece(const Vector3& from, const Vector3& to, float weight);
        float tube(const Vector3& center, float radius, float weight) const;

        Gizmo* _gizmo = nullptr;
        OverlayRendererTheme _theme;
        Entity* _parent = nullptr;
        std::shared_ptr<Layer> _fixedLayer;
        std::shared_ptr<Layer> _layer;
        std::shared_ptr<OverlayIconTextures> _iconTextures;
        std::unique_ptr<OverlayIcons> _icons;
        std::unique_ptr<MeshLineBatch> _lines;

        std::vector<std::unique_ptr<ShapePool>> _pools;
        ShapePool* _rings = nullptr;
        ShapePool* _arcs = nullptr;
        ShapePool* _arrows = nullptr;
        ShapePool* _boxes = nullptr;
        ShapePool* _arms = nullptr;
        ShapePool* _handles = nullptr;
        ShapePool* _pickDiscs = nullptr;
        ShapePool* _spheres = nullptr;
        ShapePool* _capsules = nullptr;
        std::array<ShapePool*, static_cast<size_t>(OverlayCone::Count)> _cones{};
        std::array<ShapePool*, kOverlayIconCount> _glyphs{};

        std::vector<GizmoReadout> _labels;

        // the world size of a point at unit depth, or at any depth in orthographic
        float _pixel = 0.0f;
        bool _perspective = true;
        // the draw orders taken this frame
        uint32_t _order = 0;

        // the colours of the shape or line being placed
        Color _base;
        Color _occluded;
        bool _destroyed = false;
    };
}
