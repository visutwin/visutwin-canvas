// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Rotate gizmo: a half ring per axis (the half facing the
// camera, a full ring while dragging), a ring in the view plane, and a sphere that
// orbits freely. While a ring is dragged, two angle guide lines run from the centre to
// where the drag started and to where it is now.
//
#pragma once

#include <array>
#include <memory>
#include <unordered_map>

#include "core/math/vector2.h"
#include "framework/gizmo/transformGizmo.h"

namespace visutwin::canvas
{
    class ArcShape;
    class MeshLine;
    class SphereShape;

    class RotateGizmo : public TransformGizmo
    {
    public:
        enum class RotationMode
        {
            /// The angle follows the mouse displacement from the press, along the ring's
            /// screen tangent.
            Absolute,
            /// The angle follows the mouse position around the gizmo's centre.
            Orbit
        };

        RotateGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);
        ~RotateGizmo() override;

        RotationMode rotationMode = RotationMode::Absolute;

        float xyzTubeRadius() const;
        void setXyzTubeRadius(float value);
        float xyzRingRadius() const;
        void setXyzRingRadius(float value);
        float faceTubeRadius() const;
        void setFaceTubeRadius(float value);
        float faceRingRadius() const;
        void setFaceRingRadius(float value);
        float centerRadius() const;
        void setCenterRadius(float value);
        float ringTolerance() const;
        void setRingTolerance(float value);
        float angleGuideThickness() const;
        void setAngleGuideThickness(float value);

        /// The angle guide's end points relative to the gizmo centre (in units of its scale).
        const Vector3& guideAngleStart() const { return _guideAngleStart; }
        const Vector3& guideAngleEnd() const { return _guideAngleEnd; }

        void prerender() override;
        void destroy() override;

    protected:
        Vector3 screenToPoint(float x, float y) override;
        void drawGuideLines(const Vector3& pos, const Quaternion& rot, GizmoAxis activeAxis,
                            bool activeIsPlane) override;

        /// The arc angle the drag has turned through, in degrees.
        float calculateArcAngle(const Vector3& point, float x, float y) const;

    private:
        void storeGuidePoints();
        void updateGuidePoints(float angleDelta);
        void angleGuide(bool state);
        void shapesLookAtCamera();
        void drag(bool state);
        void storeNodeRotations();
        void setNodeRotations(GizmoAxis axis, const Vector3& angleAxis, float angleDelta);
        void releaseGuideAngleLines();

        std::array<ArcShape*, 3> _arcs{};   // x, y, z
        ArcShape* _face = nullptr;
        SphereShape* _center = nullptr;

        float _selectionStartAngle = 0.0f;
        std::unordered_map<GraphNode*, Quaternion> _nodeLocalRotations;
        std::unordered_map<GraphNode*, Quaternion> _nodeRotations;
        std::unordered_map<GraphNode*, Vector3> _nodeOffsets;
        Vector2 _screenPos;
        Vector2 _screenStartPos;
        Vector3 _guideAngleStart = Vector3(0.0f);
        Vector3 _guideAngleEnd = Vector3(0.0f);
        std::array<std::unique_ptr<MeshLine>, 2> _guideAngleLines;
        Vector3 _facingDir = Vector3(0.0f);
    };
}
