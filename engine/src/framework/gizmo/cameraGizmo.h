// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Draws the frustum of each attached camera entity (other than the gizmo's own camera): its far,
// side and near edges, an up marker on the near top edge, the volume and the dashed view axis,
// with handles that drag the near and far clip planes and the field of view (or the orthographic
// height), and a camera icon.
//
#pragma once

#include "framework/gizmo/handleGizmo.h"

namespace visutwin::canvas
{
    class Entity;

    class CameraGizmo final : public HandleGizmo
    {
    public:
        CameraGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);

    protected:
        void drawShapes() override;
        void moveHandle(const GizmoHandle& handle, const Vector3& point) override;

    private:
        void draw(Entity* entity, CameraComponent* camera);
    };
}
