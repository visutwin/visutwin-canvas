// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// What one frame's rendering did, for ApplicationStats (upstream app.stats). Written by
// whoever does the work — the forward renderer, the shadow passes, the depth-only draws
// — and read and reset by Engine::fillFrameStats. It lives on the GraphicsDevice because
// every one of those writers already holds the device and most hold nothing else in
// common; the device's own draw, primitive and shader-switch counts sit beside it.
//
// Until 2026-09-25 these were members of Renderer with no write site at all: stats.cameras,
// materials, shaders, triangles, cullTime, shadowDrawCalls and every per-phase time read
// zero, and the one time that was written (sort) was truncated to whole milliseconds.
//
#pragma once

namespace visutwin::canvas
{
    struct FrameCounters
    {
        int camerasRendered = 0;    ///< cameras the frame culled for
        int materialSwitches = 0;   ///< forward draws whose material differs from the previous draw's
        int forwardDrawCalls = 0;
        int shadowDrawCalls = 0;    ///< caster draws into a shadow map, every kind of light
        int skinDrawCalls = 0;      ///< forward draws of skinned meshes
        int shadowMapUpdates = 0;   ///< shadow map faces rendered
        int lightClusters = 0;      ///< distinct cluster grids built
        int gsplats = 0;            ///< splats drawn (visible after culling)

        // Milliseconds, fractional.
        double cullTime = 0.0;
        double sortTime = 0.0;
        double forwardTime = 0.0;
        double shadowMapTime = 0.0;
        double skinTime = 0.0;
        double morphTime = 0.0;
        double lightClustersTime = 0.0;
    };
}
