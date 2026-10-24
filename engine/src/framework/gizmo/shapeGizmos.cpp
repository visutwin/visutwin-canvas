// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
#include "shapeGizmos.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "framework/gizmo/overlay/overlayRenderer.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float PI_F = std::numbers::pi_v<float>;
        constexpr float MIN_SIZE = 0.005f;
        constexpr float MIN_RADIUS = 0.005f;

        const Vector3 UP(0.0f, 1.0f, 0.0f);
        const Vector3 RIGHT(1.0f, 0.0f, 0.0f);
        const Vector3 FORWARD(0.0f, 0.0f, -1.0f);

        // the four sides of a round shape: right, forward, left, back
        std::array<Vector3, 4> sidesOf(const Quaternion& rotation)
        {
            const Vector3 right = rotation * RIGHT;
            const Vector3 forward = rotation * FORWARD;
            return {right, forward, -right, -forward};
        }
    }

    // ---------------------------------------------------------------- ShapeGizmo

    ShapeGizmo::ShapeGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer, const std::string& name)
        : HandleGizmo(camera, std::move(layer), name)
    {
        setEnabled(true);
    }

    // ---------------------------------------------------------------- BoxGizmo

    BoxGizmo::BoxGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : ShapeGizmo(camera, std::move(layer), "gizmo:box")
    {
    }

    void BoxGizmo::drawShapes()
    {
        const auto& corners = boxCorners(position, rotation, halfExtents);
        for (int i = 0; i < 8; ++i) {
            for (int bit = 1; bit < 8; bit <<= 1) {
                if (!(i & bit)) {
                    _renderer->line(corners[static_cast<size_t>(i)], corners[static_cast<size_t>(i | bit)]);
                }
            }
        }
        if (_theme.fill.a > 0.0f) {
            _renderer->box(position, rotation, halfExtents * 2.0f);
        }
        boxHandles(position, halfExtents, *this);
    }

    void BoxGizmo::startHandle(const GizmoHandle& /*handle*/)
    {
        _startPosition = position;
        _startHalfExtents = halfExtents;
    }

    void BoxGizmo::moveHandle(const GizmoHandle& handle, const Vector3& point)
    {
        resizeBox(handle, point, rotation, _startPosition, _startHalfExtents, *this, 0.0f, position, halfExtents);
    }

    // ---------------------------------------------------------------- SphereGizmo

    SphereGizmo::SphereGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : ShapeGizmo(camera, std::move(layer), "gizmo:sphere")
    {
    }

    void SphereGizmo::drawShapes()
    {
        if (_theme.fill.a > 0.0f) {
            _renderer->sphere(position, radius);
        }
        sphereOutline(position, rotation, radius);
        if (!detailed(position, radius)) {
            return;
        }
        sphereHandles(position, rotation, radius, nullptr, "radius");
    }

    void SphereGizmo::moveHandle(const GizmoHandle& handle, const Vector3& point)
    {
        radius = std::max(snapLength((point - handle.origin).dot(handle.dir)), MIN_RADIUS);
    }

    // ---------------------------------------------------------------- CapsuleGizmo

    CapsuleGizmo::CapsuleGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : ShapeGizmo(camera, std::move(layer), "gizmo:capsule")
    {
    }

    void CapsuleGizmo::drawShapes()
    {
        const Vector3& c = position;
        const float half = std::max(0.5f * height, radius);
        const Vector3 up = rotation * UP;
        const auto sides = sidesOf(rotation);

        if (_theme.fill.a > 0.0f) {
            _renderer->capsule(c, rotation, radius, 2.0f * half);
        }

        // rings where the caps meet the body
        const Vector3 top = up * (half - radius) + c;
        const Vector3 bottom = up * (radius - half) + c;
        _renderer->ring(top, rotation, radius, 0.5f, 0.35f);
        _renderer->ring(bottom, rotation, radius, 0.5f, 0.35f);
        for (const Vector3& side : sides) {
            _renderer->line(side * radius + top, side * radius + bottom, 0.5f, 0.35f);
        }
        for (int i = 0; i < 2; ++i) {
            _renderer->arc(top, sides[static_cast<size_t>(i)], up, radius, 0.0f, PI_F, 0.5f, 0.35f);
            _renderer->arc(bottom, sides[static_cast<size_t>(i)], up, radius, PI_F, 2.0f * PI_F, 0.5f, 0.35f);
        }
        outline(top, bottom, up, radius);

        if (!detailed(c, radius)) {
            return;
        }
        for (size_t i = 0; i < sides.size(); ++i) {
            const Vector3 pos = sides[i] * radius + c;
            if (handle(pos, _theme.primary, nullptr, "radius", static_cast<int>(i), c, sides[i])) {
                readoutAt(pos, "r", _theme.primary, radius);
            }
        }
        for (int i = 0; i < 2; ++i) {
            const Vector3 dir = i ? -up : up;
            const Vector3 pos = dir * half + c;
            if (handle(pos, _theme.secondary, nullptr, "height", i, c, dir)) {
                readoutAt(pos, "h", _theme.secondary, 2.0f * half);
            }
        }
    }

    void CapsuleGizmo::outline(const Vector3& top, const Vector3& bottom, const Vector3& up, const float r)
    {
        const bool persp = perspective();
        const Vector3 eye = viewPosition();
        const Vector3 view = persp ? eye - top : -viewForward();
        const float along = view.dot(up);
        const float length = top.distance(bottom);
        if (persp) {
            // the camera inside the capsule sees no outline
            const float t = std::clamp(along + length, 0.0f, length);
            if ((up * t + bottom).distance(eye) <= r) {
                return;
            }
        }

        // the sides are where the view grazes the body
        Vector3 across = up * -along + view;
        const float dist = across.length();
        const float cos = persp ? r / dist : 0.0f;
        if (!(cos < 1.0f) || dist < 1e-6f) {
            capOutline(along > 0.0f ? top : bottom, up, nullptr, r);
            return;
        }
        across = across * (1.0f / dist);
        Vector3 side = up.cross(across) * (r * std::sqrt(1.0f - cos * cos));
        const Vector3 offset = across * (r * cos);
        for (int i = 0; i < 2; ++i) {
            side = -side;
            _renderer->line(top + offset + side, bottom + offset + side, 1.1f);
        }
        capOutline(top, up, &across, r, cos);
        capOutline(bottom, -up, &across, r, cos);
    }

    void CapsuleGizmo::capOutline(const Vector3& center, const Vector3& axis, const Vector3* across, const float r,
        const float cos)
    {
        Vector3 view = perspective() ? viewPosition() - center : -viewForward();
        Vector3 outlineCenter;
        const float size = sphereSilhouette(center, r, outlineCenter);
        if (!across) {
            _renderer->circle(outlineCenter, view, size, 1.1f);
            return;
        }

        // the arc runs over the cap between the sides
        view = view.normalized();
        const Vector3 x = axis.cross(*across);
        const Vector3 y = (view * -view.dot(axis) + axis).normalized();
        const float from = std::atan2(r * cos * across->dot(y), r * std::sqrt(1.0f - cos * cos));
        _renderer->arc(outlineCenter, x, y, size, from, PI_F - from, 1.1f);
    }

    void CapsuleGizmo::moveHandle(const GizmoHandle& handle, const Vector3& point)
    {
        const float dist = std::max((point - handle.origin).dot(handle.dir), MIN_SIZE);
        if (handle.name == "radius") {
            radius = snapLength(dist);
            height = std::max(height, 2.0f * radius);
        } else {
            height = std::max(snapLength(2.0f * dist), 2.0f * radius);
        }
    }

    // ---------------------------------------------------------------- ConeGizmo

    ConeGizmo::ConeGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : ShapeGizmo(camera, std::move(layer), "gizmo:cone")
    {
    }

    void ConeGizmo::drawShapes()
    {
        const Vector3& c = position;
        const Vector3 up = rotation * UP;
        const Vector3 apex = up * (0.5f * height) + c;
        const Vector3 base = up * (-0.5f * height) + c;
        const auto sides = sidesOf(rotation);

        if (_theme.fill.a > 0.0f) {
            _renderer->cone(OverlayCone::Cone, c, rotation, Vector3(2.0f * radius, height, 2.0f * radius));
        }
        _renderer->ring(base, rotation, radius);
        for (const Vector3& side : sides) {
            _renderer->line(side * radius + base, apex, 0.5f, 0.35f);
        }
        outline(apex, base, up, radius, height);

        if (!detailed(c, radius)) {
            return;
        }
        for (size_t i = 0; i < sides.size(); ++i) {
            const Vector3 pos = sides[i] * radius + base;
            if (handle(pos, _theme.primary, nullptr, "radius", static_cast<int>(i), base, sides[i])) {
                readoutAt(pos, "r", _theme.primary, radius);
            }
        }
        if (handle(apex, _theme.secondary, nullptr, "height", 0, c, up)) {
            readoutAt(apex, "h", _theme.secondary, height);
        }
    }

    void ConeGizmo::outline(const Vector3& apex, const Vector3& base, const Vector3& up, const float r, const float h)
    {
        const Vector3 view = perspective() ? apex - viewPosition() : viewForward();

        // the outline lines graze the base ring where the view from the apex meets its plane
        const float along = view.dot(up);
        Vector3 across = up * -along + view;
        const float dist = across.length();
        const float cos = r * std::abs(along) / (h * dist);
        if (!(cos < 1.0f)) {
            return;
        }
        across = across * (along > 0.0f ? -1.0f / dist : 1.0f / dist);
        Vector3 side = up.cross(across) * (r * std::sqrt(1.0f - cos * cos));
        const Vector3 point = across * (r * cos) + base;
        for (int i = 0; i < 2; ++i) {
            side = -side;
            _renderer->line(point + side, apex, 1.1f);
        }
    }

    void ConeGizmo::moveHandle(const GizmoHandle& handle, const Vector3& point)
    {
        const float dist = std::max((point - handle.origin).dot(handle.dir), MIN_SIZE);
        if (handle.name == "radius") {
            radius = snapLength(dist);
        } else {
            height = snapLength(2.0f * dist);
        }
    }

    // ---------------------------------------------------------------- CylinderGizmo

    CylinderGizmo::CylinderGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : ShapeGizmo(camera, std::move(layer), "gizmo:cylinder")
    {
    }

    void CylinderGizmo::drawShapes()
    {
        const Vector3& c = position;
        const Vector3 up = rotation * UP;
        const auto sides = sidesOf(rotation);

        if (_theme.fill.a > 0.0f) {
            _renderer->cone(OverlayCone::Cylinder, c, rotation, Vector3(2.0f * radius, height, 2.0f * radius));
        }
        const Vector3 top = up * (0.5f * height) + c;
        const Vector3 bottom = up * (-0.5f * height) + c;
        _renderer->ring(top, rotation, radius);
        _renderer->ring(bottom, rotation, radius);
        for (const Vector3& side : sides) {
            _renderer->line(side * radius + top, side * radius + bottom, 0.5f, 0.35f);
        }
        outline(top, bottom, up, radius);

        if (!detailed(c, radius)) {
            return;
        }
        for (size_t i = 0; i < sides.size(); ++i) {
            const Vector3 pos = sides[i] * radius + c;
            if (handle(pos, _theme.primary, nullptr, "radius", static_cast<int>(i), c, sides[i])) {
                readoutAt(pos, "r", _theme.primary, radius);
            }
        }
        for (int i = 0; i < 2; ++i) {
            const Vector3 dir = i ? -up : up;
            const Vector3 pos = dir * (0.5f * height) + c;
            if (handle(pos, _theme.secondary, nullptr, "height", i, c, dir)) {
                readoutAt(pos, "h", _theme.secondary, height);
            }
        }
    }

    void CylinderGizmo::outline(const Vector3& top, const Vector3& bottom, const Vector3& up, const float r)
    {
        const bool persp = perspective();
        const Vector3 view = persp ? viewPosition() - top : -viewForward();

        // the sides are where the view grazes the body
        Vector3 across = up * -view.dot(up) + view;
        const float dist = across.length();
        const float cos = persp ? r / dist : 0.0f;
        if (!(cos < 1.0f) || dist < 1e-6f) {
            return;
        }
        across = across * (1.0f / dist);
        Vector3 side = up.cross(across) * (r * std::sqrt(1.0f - cos * cos));
        const Vector3 offset = across * (r * cos);
        for (int i = 0; i < 2; ++i) {
            side = -side;
            _renderer->line(top + offset + side, bottom + offset + side, 1.1f);
        }
    }

    void CylinderGizmo::moveHandle(const GizmoHandle& handle, const Vector3& point)
    {
        const float dist = std::max((point - handle.origin).dot(handle.dir), MIN_SIZE);
        if (handle.name == "radius") {
            radius = snapLength(dist);
        } else {
            height = snapLength(2.0f * dist);
        }
    }
}
