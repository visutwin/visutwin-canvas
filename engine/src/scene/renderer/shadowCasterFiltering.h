// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#pragma once

namespace visutwin::canvas
{
    class Camera;
    class MeshInstance;
    class RenderComponent;

    // Checks component/entity state and camera layer compatibility for shadow casting.
    bool shouldRenderShadowRenderComponent(const RenderComponent* renderComponent, const Camera* camera);

    // Checks mesh-level shadow caster rules (castShadow/material/frustum/cull/node state).
    bool shouldRenderShadowMeshInstance(MeshInstance* meshInstance, Camera* shadowCamera);
}
