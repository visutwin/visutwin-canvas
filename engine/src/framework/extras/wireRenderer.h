// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    class BoundingBox;
    class CameraComponent;
    class Immediate;
    class LightComponent;
    class LineWriter;
    class OrientedBox;
    class Scene;

    /**
     * Wireframe shapes for ONE frame, for debugging and visualisation. Every shape goes into
     * the scene's Immediate line batches and is gone once the frame has been rendered, so it
     * must be issued again every frame it should be seen: call the shapes from an update.
     *
     * The renderer holds the state its shapes are drawn with — colour, layer, depth test,
     * circle segments and an optional transform applied to every point — and the setters may
     * be called between shapes. A second set of state is simply a second renderer; renderers
     * sharing a layer and a depth-test mode draw into one batch, so having several costs
     * nothing extra. Lines are one pixel wide; WideLineRenderer draws thick lines with caps,
     * joins and dashes, as part of the scene rather than as a debugging aid.
     */
    class WireRenderer
    {
    public:
        explicit WireRenderer(Scene* scene);

        /// The colour shapes are drawn in, gamma encoded (as material colours are); its alpha
        /// is respected. White by default.
        const Color& color() const { return _color; }
        void setColor(const Color& value) { _color = value; }

        /// The layer shapes are drawn on: the Immediate layer by default.
        int layer() const { return _layer; }
        void setLayer(const int layerId) { _layer = layerId; }

        /// Whether shapes are depth tested (on by default).
        bool depthTest() const { return _depthTest; }
        void setDepthTest(const bool value) { _depthTest = value; }

        /// The segments approximating a full circle, at least 3 (20 by default).
        int segments() const { return _segments; }
        void setSegments(const int value) { _segments = value; }

        /// A matrix applied to every point of every shape, to draw in a node's space.
        const Matrix4* transform() const { return _transform ? &*_transform : nullptr; }
        void setTransform(const Matrix4& value) { _transform = value; }
        void clearTransform() { _transform.reset(); }

        void line(const Vector3& start, const Vector3& end);
        /// Separate segments from pairs of points; `colors`, one per point, or empty for the
        /// renderer's colour.
        void lines(std::span<const Vector3> positions, std::span<const Color> colors = {});
        /// Separate segments from packed xyz floats (a multiple of six), with packed rgba per
        /// vertex or none.
        void linesPacked(std::span<const float> positions, std::span<const float> colors = {});
        /// Connected segments through the points, open.
        void polyline(std::span<const Vector3> positions, std::span<const Color> colors = {});
        /// Connected segments through the points, closed back to the first.
        void loop(std::span<const Vector3> positions, std::span<const Color> colors = {});

        void boxMinMax(const Vector3& min, const Vector3& max);
        void box(const BoundingBox& box);
        void box(const OrientedBox& box);
        /// Three great circles.
        void sphere(const Vector3& center, float radius);
        void circle(const Vector3& center, const Vector3& normal, float radius);
        void cylinder(const Vector3& start, const Vector3& end, float radius);
        void capsule(const Vector3& start, const Vector3& end, float radius);
        /// `angle` is the cone's half-angle in degrees, `length` the apex to base distance.
        void cone(const Vector3& apex, const Vector3& direction, float angle, float length);
        /// A square of side `size` across `normal`, with a tick along the normal.
        void plane(const Vector3& center, const Vector3& normal, float size);
        /// Three axis-aligned strokes of length `size` crossing at the point.
        void point(const Vector3& position, float size);
        /// A line with a small cone for its head.
        void arrow(const Vector3& from, const Vector3& to);
        /// The matrix's X, Y and Z axes from its translation, red, green and blue whatever
        /// the renderer's colour.
        void axes(const Matrix4& matrix, float size);
        /// The volume a camera renders, built as the renderer builds it (the entity's position
        /// and rotation, scale-free, and the camera's projection), whether or not it renders.
        void frustum(CameraComponent* camera);
        /// The volume a (view-)projection matrix maps to clip space.
        void frustum(const Matrix4& viewProjection);
        /// A light's shape in its own colour: an omni light's range as a sphere, a spot's cone,
        /// a directional light's direction as an arrow `size` long.
        void light(const LightComponent* light, float size = 1.0f);

    private:
        LineWriter& begin(size_t vertexCount);
        [[nodiscard]] int steps() const;
        void segment(float x0, float y0, float z0, float x1, float y1, float z1);
        void segment(const Vector3& a, const Vector3& b);
        void vertex(const Vector3& p, const Color& c);
        void arc(const Vector3& center, const Vector3& u, const Vector3& v, float radius, int steps,
            float start = 0.0f, float sweep = 6.28318530718f);
        void sideLines(const Vector3& start, const Vector3& end, const Vector3& u, const Vector3& v, float radius);
        void writePoints(std::span<const Vector3> positions, std::span<const Color> colors, bool strip, bool closed);

        Immediate* _immediate = nullptr;
        LineWriter* _writer = nullptr;
        Color _color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        int _layer = LAYERID_IMMEDIATE;
        bool _depthTest = true;
        int _segments = 20;
        std::optional<Matrix4> _transform;
    };
}
