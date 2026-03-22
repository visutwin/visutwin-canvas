// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.02.2026.
//
#include "frustumUtils.h"

#include <cmath>

#include "camera.h"
#include "graphNode.h"
#include "core/math/primitives.h"

namespace visutwin::canvas
{
    bool isVisibleInCameraFrustum(Camera* camera, GraphNode* cameraNode, const BoundingBox& bounds)
    {
        if (!camera || !cameraNode) {
            return true;
        }

        const auto view = cameraNode->worldTransform().inverse();
        const auto viewProjection = camera->projectionMatrix() * view;

        Frustum frustum;
        frustum.create(viewProjection);

        const auto center = bounds.center();
        const auto extents = bounds.halfExtents();
        const float extentLen = extents.length();
        const float baseSlop = std::max(0.01f, extentLen * 0.05f);

        constexpr size_t PLANE_COUNT = 6;
        for (size_t i = 0; i < PLANE_COUNT; ++i) {
            const auto& plane = frustum.planes[i];
            const float px = plane.getX();
            const float py = plane.getY();
            const float pz = plane.getZ();
            const float pw = plane.getW();
            const float distanceToCenter =
                px * center.getX() +
                py * center.getY() +
                pz * center.getZ() +
                pw;

            const float projectedRadius =
                std::abs(px) * extents.getX() +
                std::abs(py) * extents.getY() +
                std::abs(pz) * extents.getZ();

            // Be conservative near frustum boundaries to avoid visible popping / over-cull.
            // Near plane is most sensitive when camera/projection conventions differ slightly.
            const float slop = (i == 0) ? (baseSlop * 4.0f) : baseSlop;
            if (distanceToCenter + projectedRadius < -slop) {
                return false;
            }
        }

        return true;
    }
}
