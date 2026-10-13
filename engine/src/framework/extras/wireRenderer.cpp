// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
#include "wireRenderer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

#include "core/math/vector4.h"
#include "core/shape/boundingBox.h"
#include "core/shape/orientedBox.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/entity.h"
#include "scene/camera.h"
#include "scene/immediate/immediate.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float kTwoPi = 6.28318530718f;
        constexpr float kDegToRad = 3.14159265358979f / 180.0f;

        // The 8 corners of the clip-space cube, near plane first, and the 12 edges joining them.
        constexpr std::array<float, 24> kNdcCorners = {
            -1, -1, -1,  1, -1, -1,  1, 1, -1,  -1, 1, -1,
            -1, -1,  1,  1, -1,  1,  1, 1,  1,  -1, 1,  1};
        constexpr std::array<int, 24> kFrustumEdges = {
            0, 1, 1, 2, 2, 3, 3, 0,
            4, 5, 5, 6, 6, 7, 7, 4,
            0, 4, 1, 5, 2, 6, 3, 7};

        // An orthonormal basis (u, v) perpendicular to the normalized `dir`. It cannot be
        // continuous as dir turns (no continuous tangent field covers a sphere), which only a
        // shape with a definite orientation about its axis shows: plane().
        void buildBasis(const Vector3& dir, Vector3& u, Vector3& v)
        {
            u = std::abs(dir.getX()) < 0.5f ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 1.0f, 0.0f);
            v = dir.cross(u).normalized();
            u = v.cross(dir).normalized();
        }
    }

    WireRenderer::WireRenderer(Scene* scene)
        : _immediate(scene ? scene->immediate() : nullptr)
    {
        assert(_immediate && "WireRenderer needs a scene");
    }

    LineWriter& WireRenderer::begin(const size_t vertexCount)
    {
        _writer = &_immediate->allocateLines(vertexCount, _color, _depthTest, _layer);
        return *_writer;
    }

    int WireRenderer::steps() const
    {
        return std::max(3, _segments);
    }

    void WireRenderer::segment(const float x0, const float y0, const float z0,
        const float x1, const float y1, const float z1)
    {
        if (_transform) {
            const Vector3 a = _transform->transformPoint(Vector3(x0, y0, z0));
            const Vector3 b = _transform->transformPoint(Vector3(x1, y1, z1));
            _writer->segment(a, b);
        } else {
            _writer->segment(x0, y0, z0, x1, y1, z1);
        }
    }

    void WireRenderer::segment(const Vector3& a, const Vector3& b)
    {
        segment(a.getX(), a.getY(), a.getZ(), b.getX(), b.getY(), b.getZ());
    }

    void WireRenderer::vertex(const Vector3& p, const Color& c)
    {
        const Vector3 q = _transform ? _transform->transformPoint(p) : p;
        _writer->vertex(q.getX(), q.getY(), q.getZ(), c.r, c.g, c.b, c.a);
    }

    void WireRenderer::arc(const Vector3& center, const Vector3& u, const Vector3& v, const float radius,
        const int steps, const float start, const float sweep)
    {
        const float step = sweep / static_cast<float>(steps);
        float angle = start;
        Vector3 previous = center + u * (std::cos(angle) * radius) + v * (std::sin(angle) * radius);
        for (int i = 0; i < steps; ++i) {
            angle += step;
            const Vector3 next = center + u * (std::cos(angle) * radius) + v * (std::sin(angle) * radius);
            segment(previous, next);
            previous = next;
        }
    }

    void WireRenderer::sideLines(const Vector3& start, const Vector3& end, const Vector3& u, const Vector3& v,
        const float radius)
    {
        for (int i = 0; i < 4; ++i) {
            const Vector3 offset = ((i & 1) ? v : u) * (i < 2 ? radius : -radius);
            segment(start + offset, end + offset);
        }
    }

    void WireRenderer::writePoints(const std::span<const Vector3> positions, const std::span<const Color> colors,
        const bool strip, const bool closed)
    {
        const size_t count = positions.size();
        assert((colors.empty() || colors.size() == count) && "WireRenderer: one colour per position");
        const size_t segments = strip ? (closed ? count : count - 1) : count / 2;
        begin(segments * 2);
        const bool perPoint = !colors.empty();
        for (size_t i = 0; i < segments; ++i) {
            const size_t a = strip ? i : i * 2;
            const size_t b = strip ? (i + 1) % count : i * 2 + 1;
            if (perPoint) {
                vertex(positions[a], colors[a]);
                vertex(positions[b], colors[b]);
            } else {
                segment(positions[a], positions[b]);
            }
        }
    }

    void WireRenderer::line(const Vector3& start, const Vector3& end)
    {
        begin(2);
        segment(start, end);
    }

    void WireRenderer::lines(const std::span<const Vector3> positions, const std::span<const Color> colors)
    {
        assert(positions.size() % 2 == 0 && "WireRenderer::lines needs pairs of points");
        writePoints(positions, colors, false, false);
    }

    void WireRenderer::linesPacked(const std::span<const float> positions, const std::span<const float> colors)
    {
        assert(positions.size() % 6 == 0 && "WireRenderer::linesPacked needs two xyz points per segment");
        const size_t count = positions.size() / 3;
        assert((colors.empty() || colors.size() == count * 4) && "WireRenderer::linesPacked: rgba per vertex");
        if (!_transform) {
            // Straight through, without a vertex at a time.
            _immediate->drawLineArrays(positions.data(), positions.size(),
                colors.empty() ? nullptr : colors.data(), _color, _depthTest, _layer);
            return;
        }
        begin(count);
        for (size_t i = 0; i < count; ++i) {
            const Vector3 p(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]);
            const Color c = colors.empty() ? _color
                : Color(colors[i * 4], colors[i * 4 + 1], colors[i * 4 + 2], colors[i * 4 + 3]);
            vertex(p, c);
        }
    }

    void WireRenderer::polyline(const std::span<const Vector3> positions, const std::span<const Color> colors)
    {
        if (positions.size() >= 2) {
            writePoints(positions, colors, true, false);
        }
    }

    void WireRenderer::loop(const std::span<const Vector3> positions, const std::span<const Color> colors)
    {
        if (positions.size() >= 2) {
            writePoints(positions, colors, true, true);
        }
    }

    void WireRenderer::boxMinMax(const Vector3& min, const Vector3& max)
    {
        begin(12 * 2);
        const float ax = min.getX(), ay = min.getY(), az = min.getZ();
        const float bx = max.getX(), by = max.getY(), bz = max.getZ();

        // Near face, far face, then the edges joining them.
        segment(ax, ay, az, bx, ay, az);
        segment(bx, ay, az, bx, by, az);
        segment(bx, by, az, ax, by, az);
        segment(ax, by, az, ax, ay, az);
        segment(ax, ay, bz, bx, ay, bz);
        segment(bx, ay, bz, bx, by, bz);
        segment(bx, by, bz, ax, by, bz);
        segment(ax, by, bz, ax, ay, bz);
        segment(ax, ay, az, ax, ay, bz);
        segment(bx, ay, az, bx, ay, bz);
        segment(bx, by, az, bx, by, bz);
        segment(ax, by, az, ax, by, bz);
    }

    void WireRenderer::box(const BoundingBox& box)
    {
        boxMinMax(box.center() - box.halfExtents(), box.center() + box.halfExtents());
    }

    void WireRenderer::box(const OrientedBox& box)
    {
        // A box centred at the origin of its own transform.
        const std::optional<Matrix4> outer = _transform;
        _transform = outer ? *outer * box.worldTransform() : box.worldTransform();
        boxMinMax(box.halfExtents() * -1.0f, box.halfExtents());
        _transform = outer;
    }

    void WireRenderer::sphere(const Vector3& center, const float radius)
    {
        const int n = steps();
        begin(static_cast<size_t>(n) * 3 * 2);
        const Vector3 x(1.0f, 0.0f, 0.0f);
        const Vector3 y(0.0f, 1.0f, 0.0f);
        const Vector3 z(0.0f, 0.0f, 1.0f);
        arc(center, x, y, radius, n);
        arc(center, x, z, radius, n);
        arc(center, y, z, radius, n);
    }

    void WireRenderer::circle(const Vector3& center, const Vector3& normal, const float radius)
    {
        const int n = steps();
        begin(static_cast<size_t>(n) * 2);
        Vector3 u, v;
        buildBasis(normal.normalized(), u, v);
        arc(center, u, v, radius, n);
    }

    void WireRenderer::cylinder(const Vector3& start, const Vector3& end, const float radius)
    {
        const Vector3 axis = end - start;
        if (axis.length() < 1e-6f) {
            return;
        }
        Vector3 u, v;
        buildBasis(axis.normalized(), u, v);
        const int n = steps();
        begin(static_cast<size_t>(n * 2 + 4) * 2);
        arc(start, u, v, radius, n);
        arc(end, u, v, radius, n);
        sideLines(start, end, u, v, radius);
    }

    void WireRenderer::capsule(const Vector3& start, const Vector3& end, const float radius)
    {
        const Vector3 axis = end - start;
        if (axis.length() < 1e-6f) {
            sphere(start, radius);
            return;
        }
        const Vector3 dir = axis.normalized();
        Vector3 u, v;
        buildBasis(dir, u, v);
        const int n = steps();
        const int half = std::max(2, n >> 1);
        begin(static_cast<size_t>(n * 2 + 4 + half * 4) * 2);
        arc(start, u, v, radius, n);
        arc(end, u, v, radius, n);
        sideLines(start, end, u, v, radius);

        // Hemispherical caps: two half arcs bending away from the body at each end.
        constexpr float kPi = kTwoPi * 0.5f;
        arc(end, u, dir, radius, half, 0.0f, kPi);
        arc(end, v, dir, radius, half, 0.0f, kPi);
        arc(start, u, dir, radius, half, kPi, kPi);
        arc(start, v, dir, radius, half, kPi, kPi);
    }

    void WireRenderer::cone(const Vector3& apex, const Vector3& direction, const float angle, const float length)
    {
        if (direction.length() < 1e-6f) {
            return;
        }
        const Vector3 dir = direction.normalized();
        Vector3 u, v;
        buildBasis(dir, u, v);
        const float radius = length * std::tan(std::min(angle, 89.9f) * kDegToRad);
        const Vector3 base = apex + dir * length;

        const int n = steps();
        begin(static_cast<size_t>(n + 4) * 2);
        arc(base, u, v, radius, n);
        for (int i = 0; i < 4; ++i) {
            segment(apex, base + ((i & 1) ? v : u) * (i < 2 ? radius : -radius));
        }
    }

    void WireRenderer::plane(const Vector3& center, const Vector3& normal, const float size)
    {
        begin(5 * 2);
        const Vector3 dir = normal.normalized();
        Vector3 u, v;
        buildBasis(dir, u, v);
        const float h = size * 0.5f;
        const Vector3 a = u * h;
        const Vector3 b = v * h;
        segment(center - a - b, center + a - b);
        segment(center + a - b, center + a + b);
        segment(center + a + b, center - a + b);
        segment(center - a + b, center - a - b);
        segment(center, center + dir * h);
    }

    void WireRenderer::point(const Vector3& position, const float size)
    {
        begin(3 * 2);
        const float h = size * 0.5f;
        const float x = position.getX(), y = position.getY(), z = position.getZ();
        segment(x - h, y, z, x + h, y, z);
        segment(x, y - h, z, x, y + h, z);
        segment(x, y, z - h, x, y, z + h);
    }

    void WireRenderer::arrow(const Vector3& from, const Vector3& to)
    {
        const Vector3 axis = to - from;
        const float length = axis.length();
        if (length < 1e-6f) {
            return;
        }
        const Vector3 dir = axis * (1.0f / length);
        Vector3 u, v;
        buildBasis(dir, u, v);
        const int n = steps();
        begin(static_cast<size_t>(n + 5) * 2);
        segment(from, to);

        // The head is a small cone rather than a few barbs, so it still reads as an arrow at a
        // distance where isolated lines do not.
        const float headLength = length * 0.2f;
        const float headRadius = headLength * 0.5f;
        const Vector3 head = to - dir * headLength;
        arc(head, u, v, headRadius, n);
        for (int i = 0; i < 4; ++i) {
            segment(to, head + ((i & 1) ? v : u) * (i < 2 ? headRadius : -headRadius));
        }
    }

    void WireRenderer::axes(const Matrix4& matrix, const float size)
    {
        begin(3 * 2);
        const Vector3 origin(matrix.getColumn(3));
        for (int i = 0; i < 3; ++i) {
            // Both ends of an axis share its colour, so it reads as a solid line.
            const Color color(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f, 1.0f);
            vertex(origin, color);
            vertex(origin + Vector3(matrix.getColumn(i)) * size, color);
        }
    }

    void WireRenderer::frustum(CameraComponent* camera)
    {
        Camera* cam = camera ? camera->camera() : nullptr;
        Entity* entity = camera ? camera->entity() : nullptr;
        if (!cam || !entity) {
            return;
        }
        // The view as the renderer builds it: scale-free, from the entity's world position and
        // rotation, whether or not the camera renders this frame.
        const Matrix4 view = Matrix4::trs(entity->position(), entity->rotation(), Vector3(1.0f, 1.0f, 1.0f)).inverse();
        frustum(cam->projectionMatrix() * view);
    }

    void WireRenderer::frustum(const Matrix4& viewProjection)
    {
        const Matrix4 inverse = viewProjection.inverse();
        std::array<Vector3, 8> corners;
        for (size_t i = 0; i < 8; ++i) {
            // A perspective matrix needs the w divide.
            const Vector4 p = inverse * Vector4(kNdcCorners[i * 3], kNdcCorners[i * 3 + 1], kNdcCorners[i * 3 + 2], 1.0f);
            corners[i] = Vector3(p.getX(), p.getY(), p.getZ()) * (1.0f / p.getW());
        }
        begin(12 * 2);
        for (size_t i = 0; i < kFrustumEdges.size(); i += 2) {
            segment(corners[static_cast<size_t>(kFrustumEdges[i])], corners[static_cast<size_t>(kFrustumEdges[i + 1])]);
        }
    }

    void WireRenderer::light(const LightComponent* light, const float size)
    {
        Entity* entity = light ? light->entity() : nullptr;
        if (!entity) {
            return;
        }
        const Vector3 position = entity->position();
        // A light shines along the negative Y axis of its entity.
        const Vector3 direction = (Vector3(entity->worldTransform().getColumn(1)) * -1.0f).normalized();

        const Color outer = _color;
        _color = light->color();
        switch (light->type()) {
        case LightType::LIGHTTYPE_OMNI:
        case LightType::LIGHTTYPE_POINT:
            sphere(position, light->range());
            break;
        case LightType::LIGHTTYPE_SPOT:
            cone(position, direction, light->outerConeAngle(), light->range());
            break;
        case LightType::LIGHTTYPE_DIRECTIONAL:
            arrow(position, position + direction * size);
            break;
        default:
            break;
        }
        _color = outer;
    }
}
