// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.02.2026.
//
#pragma once

#include "core/shape/boundingBox.h"

namespace visutwin::canvas
{
    class Camera;
    class GraphNode;

    // Returns true when an AABB intersects or is inside the camera frustum.
    bool isVisibleInCameraFrustum(Camera* camera, GraphNode* cameraNode, const BoundingBox& bounds);
}
