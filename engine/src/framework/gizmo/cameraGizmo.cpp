// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
#include "cameraGizmo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "core/math/matrix4.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/entity.h"
#include "framework/gizmo/overlay/overlayRenderer.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr std::array<int, 8> NEAR_EDGES = {0, 1, 1, 2, 2, 3, 3, 0};
        constexpr std::array<int, 8> FAR_EDGES = {4, 5, 5, 6, 6, 7, 7, 4};
        constexpr std::array<int, 8> SIDE_EDGES = {0, 4, 1, 5, 2, 6, 3, 7};

        // a cone's height axis turned onto the view axis, its base toward the far plane
        const Quaternion CONE_TO_VIEW = Quaternion::fromEulerAngles(90.0f, 0.0f, 0.0f);

        // the half side of a square frustum of base radius 0.5
        const float FRUSTUM_HALF_SIDE = 0.5f * std::numbers::sqrt2_v<float> * 0.5f;
        const float TRIANGLE_BASE = 0.5f * std::sqrt(3.0f);
        constexpr float TRIANGLE_HEIGHT = 0.75f;
        constexpr float MIN_CLIP = 1e-3f;
        constexpr float RAD_TO_DEG_F = 180.0f / std::numbers::pi_v<float>;

        Vector3 lerp(const Vector3& a, const Vector3& b, const float t)
        {
            return a + (b - a) * t;
        }
    }

    CameraGizmo::CameraGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : HandleGizmo(camera, std::move(layer), "gizmo:camera")
    {
    }

    void CameraGizmo::draw(Entity* entity, CameraComponent* component)
    {
        const Camera* camera = component->camera();
        const float nearClip = camera->nearClip();
        const float farClip = camera->farClip();
        const bool persp = camera->projection() == ProjectionType::Perspective;
        const Vector3 position = entity->position();

        // the frustum corners ignore the entity's scale
        const Quaternion rotation = entity->rotation();
        const Matrix4 m = Matrix4::trs(position, rotation, Vector3(1.0f));
        const std::array<Vector3, 8> points = camera->getFrustumCorners(nearClip, farClip);
        const float nearX = 0.5f * (points[0].getX() - points[3].getX());
        const float nearY = 0.5f * (points[1].getY() - points[0].getY());
        const float farX = 0.5f * (points[4].getX() - points[7].getX());
        const float farY = 0.5f * (points[5].getY() - points[4].getY());

        // the up marker spans the middle 40% of the near top edge
        const Vector3 triangle(0.8f * nearX / TRIANGLE_BASE, 1e-3f * nearY, 0.6f * nearY / TRIANGLE_HEIGHT);
        const Vector3 top = m.transformPoint(Vector3(0.5f * (points[1].getX() + points[2].getX()),
            points[1].getY() + 0.25f * triangle.getZ(), -nearClip));
        std::array<Vector3, 8> corners;
        for (size_t i = 0; i < corners.size(); ++i) {
            corners[i] = m.transformPoint(points[i]);
        }
        const Vector3 nearCenter = m.transformPoint(Vector3(0.0f, 0.0f, -nearClip));
        const Vector3 farCenter = m.transformPoint(Vector3(0.0f, 0.0f, -farClip));

        if (_theme.fill.a > 0.0f) {
            const Vector3 center = lerp(nearCenter, farCenter, 0.5f);
            if (persp) {
                const Vector3 size(farX / FRUSTUM_HALF_SIDE, farClip - nearClip, farY / FRUSTUM_HALF_SIDE);
                _renderer->cone(OverlayCone::Frustum, center, rotation * CONE_TO_VIEW, size, 1.0f, true, nearX / farX);
            } else {
                _renderer->box(center, rotation, Vector3(2.0f * farX, 2.0f * farY, farClip - nearClip));
            }
        }
        for (size_t i = 0; i < FAR_EDGES.size(); i += 2) {
            _renderer->line(corners[FAR_EDGES[i]], corners[FAR_EDGES[i + 1]], 0.8f, 0.85f);
        }
        for (size_t i = 0; i < SIDE_EDGES.size(); i += 2) {
            _renderer->line(corners[SIDE_EDGES[i]], corners[SIDE_EDGES[i + 1]], 0.7f, 0.7f);
        }
        for (size_t i = 0; i < NEAR_EDGES.size(); i += 2) {
            _renderer->line(corners[NEAR_EDGES[i]], corners[NEAR_EDGES[i + 1]], 1.1f);
        }
        _renderer->cone(OverlayCone::Triangle, top, rotation * CONE_TO_VIEW, triangle, 0.9f, false);

        // the view axis, coloured while a clip handle is active
        const Color* axis = active(entity, "nearClip", 0) ? &_theme.secondary
            : active(entity, "farClip", 0) ? &_theme.primary : nullptr;
        _renderer->dashed(nearCenter, farCenter, axis ? 1.0f : 0.55f, axis ? 1.0f : 0.6f, axis);
        placeIcon(OverlayIcon::Camera, position, entity);

        if (!detailed(farCenter, farY)) {
            return;
        }

        // the view handle sits on the far edge the field of view spans
        const Vector3 forward = gizmoNodeForward(entity);
        if (handle(nearCenter, _theme.secondary, entity, "nearClip", 0, position, forward, false, false)) {
            readoutAt(nearCenter, "N", _theme.secondary, nearClip);
        }
        if (handle(farCenter, _theme.primary, entity, "farClip", 0, position, forward, false, false)) {
            readoutAt(farCenter, "F", _theme.primary, farClip);
        }
        const bool horizontal = persp && camera->horizontalFov();
        const Vector3 edge = horizontal ? lerp(corners[4], corners[5], 0.5f) : lerp(corners[5], corners[6], 0.5f);
        const Vector3 dir = horizontal ? gizmoNodeRight(entity) : gizmoNodeUp(entity);
        if (handle(edge, _theme.tertiary, entity, persp ? "fov" : "orthoHeight", 0, farCenter, dir)) {
            if (persp) {
                readoutAt(edge, "θ", _theme.tertiary, camera->fov(), "°");
            } else {
                readoutAt(edge, "h", _theme.tertiary, camera->orthoHeight());
            }
        }
    }

    void CameraGizmo::drawShapes()
    {
        for (GraphNode* node : _nodes) {
            auto* entity = dynamic_cast<Entity*>(node);
            auto* camera = entity ? entity->findComponent<CameraComponent>() : nullptr;
            if (entity && entity->enabled() && camera && camera->camera() && camera != _camera) {
                draw(entity, camera);
            }
        }
    }

    void CameraGizmo::moveHandle(const GizmoHandle& handle, const Vector3& point)
    {
        auto* entity = dynamic_cast<Entity*>(handle.node);
        auto* component = entity ? entity->findComponent<CameraComponent>() : nullptr;
        Camera* camera = component ? component->camera() : nullptr;
        if (!camera) {
            return;
        }
        const float dist = (point - handle.origin).dot(handle.dir);
        if (handle.name == "nearClip") {
            camera->setNearClip(std::clamp(snapLength(dist), MIN_CLIP, camera->farClip() - MIN_CLIP));
        } else if (handle.name == "farClip") {
            camera->setFarClip(std::max(snapLength(dist), camera->nearClip() + MIN_CLIP));
        } else if (handle.name == "fov") {
            camera->setFov(std::clamp(snapAngle(2.0f * std::atan2(dist, camera->farClip()) * RAD_TO_DEG_F), 1.0f, 179.0f));
        } else if (handle.name == "orthoHeight") {
            camera->setOrthoHeight(std::max(snapLength(dist), MIN_CLIP));
        }
    }
}
