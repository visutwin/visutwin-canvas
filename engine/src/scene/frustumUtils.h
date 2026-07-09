// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.02.2026.
//
#pragma once

#include "core/math/primitives.h"
#include "core/shape/boundingBox.h"

namespace visutwin::canvas
{
    class Camera;
    class GraphNode;

    // Builds the camera's world-space frustum (view inverse + projection +
    // plane extraction). This is the expensive part of a visibility test —
    // build it ONCE per camera/cascade and test many bounds against it with
    // isVisibleInFrustum(); do not rebuild per mesh instance.
    Frustum buildCameraFrustum(Camera* camera, GraphNode* cameraNode);

    // AABB-vs-frustum test with conservative slop near plane boundaries.
    bool isVisibleInFrustum(const Frustum& frustum, const BoundingBox& bounds);

    // Convenience wrapper: builds the frustum and tests one AABB. Prefer the
    // build-once + isVisibleInFrustum pair inside loops.
    bool isVisibleInCameraFrustum(Camera* camera, GraphNode* cameraNode, const BoundingBox& bounds);
}
