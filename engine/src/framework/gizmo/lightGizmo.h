// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Draws the shape of each attached light entity: a directional light's sun disc, rays and arrow
// (sized on screen), an omni light's range sphere, a spot light's cone with its range cap and
// inner cone, and an area light's rectangle, disk or sphere. Handles drag the range, the cone
// angles and the area size (the entity's scale), with an optional dashed line down to the ground
// and the light's colour for the drawing.
//
#pragma once

#include "framework/gizmo/handleGizmo.h"

namespace visutwin::canvas
{
    class Entity;
    class LightComponent;

    class LightGizmo final : public HandleGizmo
    {
    public:
        LightGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);

        /// A dashed line from a local light down to the ground (y = 0), with a ring at its foot.
        bool groundLine = false;
        /// Draws each light in its colour instead of the theme's.
        bool useLightColor = false;

    protected:
        void drawShapes() override;
        void startHandle(const GizmoHandle& handle) override;
        void moveHandle(const GizmoHandle& handle, const Vector3& point) override;

    private:
        void ground(const Vector3& position);
        void rays(Entity* entity, const Vector3* starts, int count, float length, float weight = 0.625f,
            float alpha = 0.55f);
        static Vector3 circlePoint(Entity* entity, const Vector3& center, float radius, float angle);
        bool isArea(const LightComponent& light) const;
        bool drawArea(Entity* entity, const LightComponent& light);
        void drawSpot(Entity* entity, const LightComponent& light);
        void drawDirectional(Entity* entity);
        void drawOmni(Entity* entity, const LightComponent& light);

        Vector3 _startScale = Vector3(1.0f);
        Vector3 _startWorldScale = Vector3(1.0f);
        Vector3 _startParentScale = Vector3(1.0f);
    };
}
