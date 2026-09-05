// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2026.
//
#pragma once

namespace visutwin::canvas
{
    class Light;

    /**
     * One-pass omni shadow caster classification (upstream cullShadowCastersOmni).
     *
     * An omni light draws six shadow faces. Culling each face independently sweeps
     * the whole scene six times and builds six frustums. This sweeps once and
     * classifies every caster into the faces it touches, writing the result into
     * each face's LightRenderData::visibleCasters for the passes to consume.
     *
     * The saving is not only the five extra sweeps: the six face frusta share their
     * near, far and side-plane slope, and their axes are the world axes, so a
     * caster's AABB is tested against all six with a handful of comparisons on
     * light-space coordinates rather than six by twenty-four plane tests.
     *
     * REQUIRES the face cameras to look down +X, -X, +Y, -Y, +Z, -Z in that order —
     * `LightCamera::pointLightRotations` puts them there, and `omniFaceAxisTests`
     * in the test suite is what keeps them there.
     *
     * Safe to call for any light; does nothing unless the light is an omni that
     * casts shadows.
     */
    void cullShadowCastersOmni(Light* light);
}
