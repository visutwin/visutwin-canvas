// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// The shape gizmos: a box, sphere, capsule, cone or cylinder at a world `position` and
// `rotation`, its size edited by dragging its handles. They draw without attached nodes, so they
// are enabled from construction; read the edited values back from the gizmo.
//
#pragma once

#include "framework/gizmo/handleGizmo.h"

namespace visutwin::canvas
{
    class ShapeGizmo : public HandleGizmo
    {
    public:
        ShapeGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer, const std::string& name = "gizmo:shape");

        /// The world position of the shape's centre.
        Vector3 position = Vector3(0.0f);
        /// The world rotation of the shape.
        Quaternion rotation;

    protected:
        bool requiresNodes() const override { return false; }
    };

    /// A box of `halfExtents`, resized by its face and corner handles.
    class BoxGizmo final : public ShapeGizmo, public BoxHandleOptions
    {
    public:
        BoxGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);

        Vector3 halfExtents = Vector3(0.5f, 0.5f, 0.5f);

    protected:
        void drawShapes() override;
        void startHandle(const GizmoHandle& handle) override;
        void moveHandle(const GizmoHandle& handle, const Vector3& point) override;

    private:
        Vector3 _startPosition = Vector3(0.0f);
        Vector3 _startHalfExtents = Vector3(0.0f);
    };

    /// A sphere of `radius`, resized by the handles at the ends of its axes.
    class SphereGizmo final : public ShapeGizmo
    {
    public:
        SphereGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);

        float radius = 0.5f;

    protected:
        void drawShapes() override;
        void moveHandle(const GizmoHandle& handle, const Vector3& point) override;
    };

    /// A capsule along its y axis, `height` caps included (never below twice the radius).
    class CapsuleGizmo final : public ShapeGizmo
    {
    public:
        CapsuleGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);

        float radius = 0.5f;
        float height = 2.0f;

    protected:
        void drawShapes() override;
        void moveHandle(const GizmoHandle& handle, const Vector3& point) override;

    private:
        void outline(const Vector3& top, const Vector3& bottom, const Vector3& up, float radius);
        void capOutline(const Vector3& center, const Vector3& axis, const Vector3* across, float radius,
            float cos = 0.0f);
    };

    /// A cone along its y axis, its apex at `height` / 2 and its base of `radius` at -`height` / 2.
    class ConeGizmo final : public ShapeGizmo
    {
    public:
        ConeGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);

        float radius = 0.5f;
        float height = 1.0f;

    protected:
        void drawShapes() override;
        void moveHandle(const GizmoHandle& handle, const Vector3& point) override;

    private:
        void outline(const Vector3& apex, const Vector3& base, const Vector3& up, float radius, float height);
    };

    /// A cylinder along its y axis.
    class CylinderGizmo final : public ShapeGizmo
    {
    public:
        CylinderGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);

        float radius = 0.5f;
        float height = 2.0f;

    protected:
        void drawShapes() override;
        void moveHandle(const GizmoHandle& handle, const Vector3& point) override;

    private:
        void outline(const Vector3& top, const Vector3& bottom, const Vector3& up, float radius);
    };
}
