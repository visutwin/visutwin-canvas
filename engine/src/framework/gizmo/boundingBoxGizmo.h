// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// Corner brackets around the world-space bounds of the attached nodes and their descendants,
// with dashed edges, a volume, and face and corner handles that resize the attached nodes
// (moving each by the resize and scaling each local axis by the factor of its nearest world
// axis), labelled with the width, height and depth, and an origin marker on each node.
//
// The bounds are gathered every frame, before rendering, from the visible mesh instances of
// every enabled render component under the attached nodes (a splat cloud's instance included,
// which carries its extent). Use a layer that keeps the scene depth (Gizmo::createLayer with
// `depth`), so the parts the scene hides show faint.
//
#pragma once

#include <vector>

#include "core/shape/boundingBox.h"
#include "framework/gizmo/handleGizmo.h"

namespace visutwin::canvas
{
    class BoundingBoxGizmo final : public HandleGizmo, public BoxHandleOptions
    {
    public:
        BoundingBoxGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);

        /// The dashed edges between the brackets.
        bool edges = true;
        /// The length of a bracket arm, as a fraction of its edge's half length.
        float armLength = 0.3f;
        /// The space between the bounds and the brackets, in world units on each side.
        float padding = 0.01f;
        /// The thickness of a bracket arm, in points.
        float armThickness = 2.0f;

        /// Whether any attached node has bounds this frame.
        bool hasBounds() const { return !_empty; }
        /// The padded box drawn this frame.
        const Vector3& center() const { return _center; }
        const Vector3& halfExtents() const { return _halfExtents; }

    protected:
        void drawShapes() override;
        void startHandle(const GizmoHandle& handle) override;
        void moveHandle(const GizmoHandle& handle, const Vector3& point) override;

    private:
        void add(const BoundingBox& aabb);
        void addNode(GraphNode* node);
        void drawBrackets(const std::array<Vector3, 8>& corners);

        BoundingBox _bounds;
        bool _empty = true;
        Vector3 _center = Vector3(0.0f);
        Vector3 _halfExtents = Vector3(0.0f);
        Vector3 _startCenter = Vector3(0.0f);
        Vector3 _startHalfExtents = Vector3(0.0f);
        std::vector<Vector3> _startPositions;
        std::vector<Vector3> _startScales;
    };
}
