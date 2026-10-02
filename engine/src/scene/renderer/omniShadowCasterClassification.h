// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2026
//
#pragma once

#include <vector>

namespace visutwin::canvas
{
    class Light;
    struct ShadowCasterBounds;

    /**
     * One-pass omni shadow caster classification.
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
     * `casters` is the scene's caster list with the light-independent rules already
     * applied and each caster's bounds beside it (collectLightIndependentShadowCasters):
     * the caller collects it once for every light it culls, so what runs per light is
     * the bounds test alone, over one array. `frame` is
     * the device's renderVersion, stamped on each face's list so a pass uses it only in
     * the frame that prepared it.
     *
     * Safe to call for any light; does nothing unless the light is an omni that
     * casts shadows.
     */
    void cullShadowCastersOmni(Light* light, const std::vector<ShadowCasterBounds>& casters, int frame);
}
