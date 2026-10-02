// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Arrows along X, Y and Z, plane handles at
// their intersections and a centre sphere.
//
//     auto layer = Gizmo::createLayer(engine);
//     TranslateGizmo gizmo(cameraComponent, layer);
//     gizmo.attach(entity);
//
#pragma once

#include <unordered_map>

#include "framework/gizmo/transformGizmo.h"

namespace visutwin::canvas
{
    class ArrowShape;
    class PlaneShape;
    class SphereShape;

    class TranslateGizmo : public TransformGizmo
    {
    public:
        TranslateGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);
        ~TranslateGizmo() override;

        /// Flips the planes to face the camera.
        bool flipPlanes = true;

        float axisGap() const;
        void setAxisGap(float value);
        float axisLineThickness() const;
        void setAxisLineThickness(float value);
        float axisLineLength() const;
        void setAxisLineLength(float value);
        float axisLineTolerance() const;
        void setAxisLineTolerance(float value);
        float axisArrowThickness() const;
        void setAxisArrowThickness(float value);
        float axisArrowLength() const;
        void setAxisArrowLength(float value);
        float axisPlaneSize() const;
        void setAxisPlaneSize(float value);
        float axisPlaneGap() const;
        void setAxisPlaneGap(float value);
        float axisCenterSize() const;
        void setAxisCenterSize(float value);

        void prerender() override;

    protected:
        Vector3 screenToPoint(float x, float y) override;
        void drawGuideLines(const Vector3& pos, const Quaternion& rot, GizmoAxis activeAxis,
                            bool activeIsPlane) override;

    private:
        void shapesLookAtCamera();
        void drag(bool state);
        void storeNodePositions();
        void setNodePositions(const Vector3& translateDelta);

        SphereShape* _center = nullptr;
        std::array<PlaneShape*, 3> _planes{};   // yz, xz, xy
        std::array<ArrowShape*, 3> _arrows{};   // x, y, z

        std::unordered_map<GraphNode*, Vector3> _nodeLocalPositions;
        std::unordered_map<GraphNode*, Vector3> _nodePositions;
    };
}
