// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Box-tipped lines along X, Y and Z, plane handles
// at their intersections and a centre box for uniform scaling. Always in local space.
//
#pragma once

#include <array>
#include <limits>
#include <unordered_map>

#include "framework/gizmo/transformGizmo.h"

namespace visutwin::canvas
{
    class BoxLineShape;
    class BoxShape;
    class PlaneShape;

    class ScaleGizmo : public TransformGizmo
    {
    public:
        ScaleGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);
        ~ScaleGizmo() override;

        /// Disallowed: the scale gizmo always works in local space.
        void setCoordSpace(GizmoSpace /*value*/) override {}

        /// Uniform scaling for the planes.
        bool uniform() const { return _uniform; }
        void setUniform(const bool value) { _uniform = value; }

        /// Flips the planes to face the camera.
        bool flipPlanes = true;

        /// The lower bound for scaling.
        Vector3 lowerBoundScale = Vector3(-std::numeric_limits<float>::infinity());

        float axisGap() const;
        void setAxisGap(float value);
        float axisLineThickness() const;
        void setAxisLineThickness(float value);
        float axisLineLength() const;
        void setAxisLineLength(float value);
        float axisLineTolerance() const;
        void setAxisLineTolerance(float value);
        float axisBoxSize() const;
        void setAxisBoxSize(float value);
        float axisPlaneSize() const;
        void setAxisPlaneSize(float value);
        float axisPlaneGap() const;
        void setAxisPlaneGap(float value);
        float axisCenterSize() const;
        void setAxisCenterSize(float value);

        void prerender() override;

    protected:
        Vector3 screenToPoint(float x, float y) override;

    private:
        void shapesLookAtCamera();
        void drag(bool state);
        void storeNodeScales();
        void setNodeScales(const Vector3& scaleDelta);

        BoxShape* _center = nullptr;
        std::array<PlaneShape*, 3> _planes{};     // yz, xz, xy
        std::array<BoxLineShape*, 3> _lines{};    // x, y, z
        bool _uniform = false;
        std::unordered_map<GraphNode*, Vector3> _nodeScales;
    };
}
