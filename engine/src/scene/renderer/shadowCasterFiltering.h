// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#pragma once

#include <vector>

#include "core/math/primitives.h"

namespace visutwin::canvas
{
    class Camera;
    class MeshInstance;
    class RenderComponent;

    // Checks component/entity state and camera layer compatibility for shadow casting.
    bool shouldRenderShadowRenderComponent(const RenderComponent* renderComponent, const Camera* camera);

    // Collects every shadow caster in the scene: each enabled RenderComponent's mesh
    // instances plus the batch mesh instances, which belong to no RenderComponent and
    // would otherwise cast nothing. Appends; does not clear.
    void collectShadowCasters(std::vector<MeshInstance*>& casters);

    // The mesh-level caster rules that do NOT depend on a camera: castShadow, node
    // state, material transparency (with the alpha-test and dithered-shadow
    // exceptions) and the presence of geometry. Split out so a caller that does its
    // own visibility test — the omni classification, which tests six faces at once —
    // pays these once rather than once per face.
    bool shouldRenderShadowMeshInstanceIgnoringVisibility(MeshInstance* meshInstance);

    // Checks mesh-level shadow caster rules (castShadow/material/frustum/cull/node state).
    bool shouldRenderShadowMeshInstance(MeshInstance* meshInstance, Camera* shadowCamera);

    // Prebuilt-frustum variant: callers looping over many casters should build
    // the shadow camera's frustum once (buildCameraFrustum) and pass it here.
    bool shouldRenderShadowMeshInstance(MeshInstance* meshInstance, Camera* shadowCamera,
        const Frustum& shadowFrustum);
}
