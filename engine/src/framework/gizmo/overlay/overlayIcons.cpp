// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
#include "overlayIcons.h"

#include "framework/components/camera/cameraComponent.h"
#include "framework/entity.h"
#include "framework/gizmo/gizmo.h"
#include "framework/gizmo/gizmoConstants.h"
#include "framework/gizmo/gizmoMath.h"
#include "framework/gizmo/overlay/overlayRenderer.h"
#include "framework/gizmo/shape/shape.h"
#include "scene/camera.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        // the backdrop radius as a factor of the icon size
        constexpr float ICON_BACKDROP_RADIUS = 7.0f / 12.0f;
    }

    OverlayIcons::OverlayIcons(OverlayRenderer* renderer, Gizmo* gizmo)
        : _renderer(renderer), _gizmo(gizmo)
    {
    }

    void OverlayIcons::add(const OverlayIcon kind, const Vector3& position, GraphNode* node)
    {
        const CameraComponent* camera = _gizmo->camera();
        if (!camera || !camera->entity() || !camera->camera()) {
            return;
        }
        Entity* entity = camera->entity();
        const float size = _renderer->pixels(position, _renderer->iconSize);
        const Vector3 forward = (-Vector3(entity->worldTransform().getColumn(2))).normalized();

        // the backdrop sits a pixel behind the glyph, away from the camera
        Vector3 away = camera->camera()->projection() == ProjectionType::Perspective
            ? (position - entity->position()).normalized() : forward;
        const Vector3 behind = away * _renderer->pixels(position, 1.0f) + position;
        const Color& color = _renderer->theme().backdrop ? *_renderer->theme().backdrop : GIZMO_COLOR_BACKDROP;
        Shape* backdrop = _renderer->disc(behind, gizmoRotationBetween(Vector3(0.0f, 1.0f, 0.0f), forward),
            2.0f * ICON_BACKDROP_RADIUS * size, color.a, &color);

        // the glyph's xz plane turned to face the camera, its top up the screen
        static const Quaternion glyphToView = Quaternion::fromEulerAngles(90.0f, 0.0f, 0.0f);
        Shape* glyph = _renderer->glyph(kind, position, entity->rotation() * glyphToView, size);

        Placed& record = _placed.next();
        record.shape = backdrop;
        record.glyph = glyph;
        record.node = node;
    }

    GraphNode* OverlayIcons::pick(const MeshInstance* meshInstance) const
    {
        if (!meshInstance) {
            return nullptr;
        }
        for (size_t i = 0; i < _placed.count; ++i) {
            const Placed& placed = *_placed.items()[i];
            const auto first = [](const Shape* shape) {
                return shape && !shape->meshInstances().empty() ? shape->meshInstances().front() : nullptr;
            };
            if (first(placed.shape) == meshInstance || first(placed.glyph) == meshInstance) {
                return placed.node;
            }
        }
        return nullptr;
    }
}
