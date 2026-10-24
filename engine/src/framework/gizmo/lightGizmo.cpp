// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
#include "lightGizmo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/gizmo/overlay/overlayRenderer.h"
#include "scene/camera.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float PI_F = std::numbers::pi_v<float>;
        constexpr float DEG_TO_RAD_F = PI_F / 180.0f;
        constexpr float RAD_TO_DEG_F = 180.0f / PI_F;

        const std::array<Vector3, 4> RECT = {
            Vector3(-0.5f, 0.0f, -0.5f), Vector3(0.5f, 0.0f, -0.5f), Vector3(0.5f, 0.0f, 0.5f), Vector3(-0.5f, 0.0f, 0.5f),
        };

        // an arrow along the light's -y, the way it shines
        const Quaternion FLIP = Quaternion::fromEulerAngles(180.0f, 0.0f, 0.0f);

        constexpr int RAYS = 8;
        constexpr float GROUND_RADIUS = 0.12f;

        // a factor of the fill alpha of an area light's face
        constexpr float FACE_OPACITY = 3.0f;
        constexpr float MIN_SIZE = 0.01f;
    }

    LightGizmo::LightGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : HandleGizmo(camera, std::move(layer), "gizmo:light")
    {
    }

    void LightGizmo::ground(const Vector3& pos)
    {
        if (!groundLine || std::abs(pos.getY()) < GROUND_RADIUS) {
            return;
        }
        const Vector3 foot(pos.getX(), 0.0f, pos.getZ());
        _renderer->dashed(pos, foot, 0.625f, 0.7f);
        _renderer->circle(foot, Vector3(0.0f, 1.0f, 0.0f), GROUND_RADIUS, 0.625f, 0.7f);
    }

    void LightGizmo::rays(Entity* entity, const Vector3* starts, const int count, const float length,
        const float weight, const float alpha)
    {
        const Vector3 along = gizmoNodeUp(entity) * -length;
        for (int i = 0; i < count; ++i) {
            _renderer->line(starts[i], starts[i] + along, weight, alpha);
        }
    }

    Vector3 LightGizmo::circlePoint(Entity* entity, const Vector3& center, const float radius, const float angle)
    {
        return gizmoNodeRight(entity) * (std::cos(angle) * radius) + gizmoNodeForward(entity) * (std::sin(angle) * radius)
            + center;
    }

    bool LightGizmo::isArea(const LightComponent& light) const
    {
        // clustered lighting treats local area lights as punctual unless area lights are enabled
        const Scene* scene = _engine ? _engine->scene().get() : nullptr;
        return light.shape() != LightShape::LIGHTSHAPE_PUNCTUAL &&
            (!scene || !scene->clusteredLightingEnabled() || light.type() == LightType::LIGHTTYPE_DIRECTIONAL ||
                scene->lighting().areaLightsEnabled);
    }

    bool LightGizmo::drawArea(Entity* entity, const LightComponent& light)
    {
        if (!isArea(light)) {
            return false;
        }
        const LightShape shape = light.shape();
        const Matrix4& wtm = entity->worldTransform();
        const Vector3 pos = entity->position();
        const Quaternion rot = entity->rotation();
        const Vector3 scale = wtm.getScale();
        const float sx = scale.getX();
        const float sy = scale.getY();
        const float sz = scale.getZ();
        const bool volume = _theme.fill.a > 0.0f;
        const float radius = shape == LightShape::LIGHTSHAPE_RECT ? 0.5f * std::max(sx, sz)
            : shape == LightShape::LIGHTSHAPE_DISK ? 0.25f * (sx + sz) : 0.5f * std::max({sx, sy, sz});
        const bool isDetailed = detailed(pos, radius);

        switch (shape) {
            case LightShape::LIGHTSHAPE_RECT: {
                std::array<Vector3, 4> rect;
                for (size_t i = 0; i < RECT.size(); ++i) {
                    rect[i] = wtm.transformPoint(RECT[i]);
                }
                if (volume) {
                    _renderer->box(pos, rot, Vector3(sx, 1e-3f * (sx + sz), sz), FACE_OPACITY);
                }
                for (size_t i = 0; i < rect.size(); ++i) {
                    _renderer->line(rect[i], rect[(i + 1) % rect.size()], 1.1f);
                }
                rays(entity, rect.data(), static_cast<int>(rect.size()), 0.17f * (sx + sz));

                // the edge midpoints alternate between depth (z) and width (x)
                if (isDetailed) {
                    for (size_t i = 0; i < rect.size(); ++i) {
                        const Vector3 mid = (rect[i] + rect[(i + 1) % rect.size()]) * 0.5f;
                        const Vector3 dir = (mid - pos).normalized();
                        const bool width = i & 1;
                        const Color& color = width ? _theme.primary : _theme.tertiary;
                        if (handle(mid, color, entity, width ? "width" : "depth", static_cast<int>(i), pos, dir)) {
                            readoutAt(mid, width ? "W" : "D", color, width ? sx : sz);
                        }
                    }
                }
                break;
            }
            case LightShape::LIGHTSHAPE_DISK: {
                if (volume) {
                    _renderer->cone(OverlayCone::Disk, pos, rot, Vector3(2.0f * radius, 2e-3f * radius, 2.0f * radius),
                        FACE_OPACITY);
                }
                _renderer->ring(pos, rot, radius, 1.1f);
                std::array<Vector3, RAYS> starts;
                for (int i = 0; i < RAYS; ++i) {
                    starts[static_cast<size_t>(i)] = circlePoint(entity, pos, radius, 2.0f * PI_F * i / RAYS);
                }
                rays(entity, starts.data(), RAYS, 0.67f * radius);
                if (isDetailed) {
                    for (int i = 0; i < 4; ++i) {
                        const Vector3 rim = circlePoint(entity, pos, radius, 0.5f * PI_F * i);
                        if (handle(rim, _theme.primary, entity, "radius", i, pos, (rim - pos).normalized())) {
                            readoutAt(rim, "⌀", _theme.primary, 2.0f * radius);
                        }
                    }
                }
                break;
            }
            case LightShape::LIGHTSHAPE_SPHERE: {
                if (volume) {
                    _renderer->sphere(pos, radius, FACE_OPACITY);
                }
                sphereOutline(pos, rot, radius);
                if (isDetailed) {
                    sphereHandles(pos, rot, radius, entity, "radius");
                }
                break;
            }
            default:
                return true;
        }
        if (shape != LightShape::LIGHTSHAPE_SPHERE) {
            _renderer->arrow(pos, rot * FLIP, 0.45f * (sx + sz));
        }
        if (light.type() != LightType::LIGHTTYPE_DIRECTIONAL) {
            ground(pos);
        }
        placeIcon(OverlayIcon::Light, pos, entity);
        return true;
    }

    void LightGizmo::drawSpot(Entity* entity, const LightComponent& light)
    {
        const float range = light.range();
        const float innerConeAngle = light.innerConeAngle();
        const float outerConeAngle = light.outerConeAngle();
        const Vector3 apex = entity->position();
        const Vector3 dir = -gizmoNodeUp(entity);
        const float outer = outerConeAngle * DEG_TO_RAD_F;
        const float inner = innerConeAngle * DEG_TO_RAD_F;
        const float length = range * std::cos(outer);
        const float radius = range * std::sin(outer);
        const Vector3 center = dir * length + apex;
        const Vector3 innerCenter = dir * (range * std::cos(inner)) + apex;
        const float innerRadius = range * std::sin(inner);

        if (_theme.fill.a > 0.0f && length > 1e-4f) {
            const Vector3 mid = dir * (0.5f * length) + apex;
            _renderer->cone(OverlayCone::Cone, mid, entity->rotation(), Vector3(2.0f * radius, length, 2.0f * radius));
        }

        // edges along the entity's x and z axes
        for (int i = 0; i < 4; ++i) {
            _renderer->line(apex, circlePoint(entity, center, radius, 0.5f * PI_F * i));
        }
        _renderer->circle(center, dir, radius, 1.1f);
        if (innerConeAngle > 0.0f && innerConeAngle < outerConeAngle) {
            _renderer->dashedCircle(innerCenter, dir, innerRadius, 0.625f, 0.8f);
        }

        // the range cap's arcs, coloured while their angle handle is active
        for (int i = 0; i < 4; ++i) {
            const Vector3 radial = circlePoint(entity, Vector3(0.0f), 1.0f, 0.5f * PI_F * i);
            const Color* color = active(entity, "outerConeAngle", i) ? &_theme.tertiary
                : active(entity, "innerConeAngle", i) ? &_theme.secondary : nullptr;
            _renderer->arc(apex, dir, radial, range, 0.0f, outer, color ? 1.0f : 0.625f, color ? 1.0f : 0.6f, color);
        }
        const Vector3 tip = dir * range + apex;
        const bool ranging = active(entity, "range", 0);
        _renderer->dashed(apex, tip, ranging ? 1.0f : 0.55f, ranging ? 1.0f : 0.6f, ranging ? &_theme.primary : nullptr);
        placeIcon(OverlayIcon::Light, apex, entity);
        ground(apex);

        if (!detailed(center, radius)) {
            return;
        }

        // the angle handles swing along the range cap's arcs
        if (handle(tip, _theme.primary, entity, "range", 0, apex, dir, false, false)) {
            readoutAt(tip, "r", _theme.primary, range);
        }
        for (int i = 0; i < 4; ++i) {
            const float angle = 0.5f * PI_F * i;
            const Vector3 radial = circlePoint(entity, Vector3(0.0f), 1.0f, angle);
            const Vector3 normal = radial.cross(dir).normalized();
            const Vector3 rim = circlePoint(entity, center, radius, angle);
            if (handle(rim, _theme.tertiary, entity, "outerConeAngle", i, apex, normal, true)) {
                readoutAt(rim, "θo", _theme.tertiary, outerConeAngle, "°");
            }
            if (innerConeAngle > 0.0f) {
                const Vector3 ring = circlePoint(entity, innerCenter, innerRadius, angle);
                if (handle(ring, _theme.secondary, entity, "innerConeAngle", i, apex, normal, true)) {
                    readoutAt(ring, "θi", _theme.secondary, innerConeAngle, "°");
                }
            }
        }
    }

    void LightGizmo::drawDirectional(Entity* entity)
    {
        const Vector3 pos = entity->position();
        const Quaternion rot = entity->rotation();
        const float scale = getScreenScale(pos);
        const float radius = 0.24f * scale;
        _renderer->ring(pos, rot, radius);
        std::array<Vector3, RAYS> starts;
        for (int i = 0; i < RAYS; ++i) {
            starts[static_cast<size_t>(i)] = circlePoint(entity, pos, radius, 2.0f * PI_F * i / RAYS);
        }
        rays(entity, starts.data(), RAYS, 0.58f * scale, 0.7f, 0.75f);
        _renderer->arrow(pos, rot * FLIP, scale);

        // the icon sits on the light, moved toward the camera past the arrow so it draws on top
        const Vector3 toward = perspective() ? (cameraPosition() - pos).normalized() : -cameraForward();
        placeIcon(OverlayIcon::Light, toward * scale + pos, entity);
    }

    void LightGizmo::drawOmni(Entity* entity, const LightComponent& light)
    {
        const float range = light.range();
        const Vector3 pos = entity->position();
        sphereOutline(pos, Quaternion(), range);
        ground(pos);
        placeIcon(OverlayIcon::Light, pos, entity);
        if (detailed(pos, range)) {
            sphereHandles(pos, Quaternion(), range, entity, "range");
        }
    }

    void LightGizmo::drawShapes()
    {
        for (GraphNode* node : _nodes) {
            auto* entity = dynamic_cast<Entity*>(node);
            const auto* light = entity ? entity->findComponent<LightComponent>() : nullptr;
            if (!entity || !entity->enabled() || !light) {
                continue;
            }
            _renderer->tint = useLightColor ? &light->color() : nullptr;
            if (drawArea(entity, *light)) {
                continue;
            }
            switch (light->type()) {
                case LightType::LIGHTTYPE_DIRECTIONAL:
                    drawDirectional(entity);
                    break;
                case LightType::LIGHTTYPE_OMNI:
                case LightType::LIGHTTYPE_POINT:
                    drawOmni(entity, *light);
                    break;
                case LightType::LIGHTTYPE_SPOT:
                    drawSpot(entity, *light);
                    break;
            }
        }
        _renderer->tint = nullptr;
    }

    void LightGizmo::startHandle(const GizmoHandle& handle)
    {
        auto* entity = dynamic_cast<Entity*>(handle.node);
        if (!entity) {
            return;
        }
        _startScale = entity->localScale();
        _startWorldScale = entity->worldTransform().getScale();
        GraphNode* parent = entity->parent();
        _startParentScale = parent ? parent->worldTransform().getScale() : Vector3(1.0f);
    }

    void LightGizmo::moveHandle(const GizmoHandle& handle, const Vector3& point)
    {
        auto* entity = dynamic_cast<Entity*>(handle.node);
        auto* light = entity ? entity->findComponent<LightComponent>() : nullptr;
        if (!light) {
            return;
        }
        const Vector3 offset = point - handle.origin;
        const float dist = offset.dot(handle.dir);
        const Vector3& start = _startScale;
        const Vector3& world = _startWorldScale;
        const Vector3& parent = _startParentScale;
        if (handle.name == "range") {
            light->setRange(std::max(snapLength(dist), MIN_SIZE));
        } else if (handle.name == "outerConeAngle" || handle.name == "innerConeAngle") {
            const Vector3 dir = -gizmoNodeUp(entity);
            const float along = offset.dot(dir);
            const float across = (dir * -along + offset).length();
            const float angle = snapAngle(std::atan2(across, along) * RAD_TO_DEG_F);
            if (handle.name == "outerConeAngle") {
                light->setOuterConeAngle(std::clamp(angle, 1.0f, 89.0f));
                light->setInnerConeAngle(std::min(light->innerConeAngle(), light->outerConeAngle()));
            } else {
                light->setInnerConeAngle(std::clamp(angle, 0.0f, light->outerConeAngle()));
            }
        } else if (handle.name == "width") {
            entity->setLocalScale(std::max(snapLength(2.0f * dist), MIN_SIZE) / parent.getX(), start.getY(), start.getZ());
        } else if (handle.name == "depth") {
            entity->setLocalScale(start.getX(), start.getY(), std::max(snapLength(2.0f * dist), MIN_SIZE) / parent.getZ());
        } else if (handle.name == "radius") {
            // a disk's radius is a quarter of its width plus depth and snaps by diameter
            const bool disk = light->shape() == LightShape::LIGHTSHAPE_DISK;
            const float radius = disk ? 0.25f * (world.getX() + world.getZ())
                : 0.5f * std::max({world.getX(), world.getY(), world.getZ()});
            const float snapped = disk ? 0.5f * snapLength(2.0f * dist) : snapLength(dist);
            const float factor = std::max(snapped, MIN_SIZE) / radius;
            entity->setLocalScale(start.getX() * factor, disk ? start.getY() : start.getY() * factor,
                start.getZ() * factor);
        }
    }
}
