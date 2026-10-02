// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Port of upstream gizmos/transform-scale.
//
// A default white box at the origin, lit by a default directional light at euler
// (0, 0, -60) with ambient 0.2, over a 4x4 grid. A scale gizmo, drawn in its own
// depth-cleared layer from Gizmo::createLayer, is attached to the box; its size is
// 1024 / the canvas height, kept up to date as the window resizes, and the orbit camera
// (focused on the origin, zoom 2..10, pitch +/-89.999) stops responding while the gizmo
// holds the pointer (the 'gizmo:pointer' event).
//
// DEVIATIONS:
// - No controls panel: upstream's initial values apply (snap off, local space — the
//   scale gizmo's only one — uniform off, the default theme, drag mode 'selected',
//   perspective at 45 degrees).
// - Upstream's Grid script is a pristine-grid shader on a blended plane; grid.h draws
//   it with wide lines instead, over the scaled (4, 1, 4) extent.
// - The example harness re-enables the camera controls at the top of every frame (for
//   its HUD), so the gizmo's pointer hold is re-applied in update().
//
// The scene is the one all three transform gizmo examples share (transformGizmoExample.h);
// this file chooses the gizmo.
//
#include "../transformGizmoExample.h"
#include "framework/gizmo/scaleGizmo.h"

using namespace visutwin::canvas;

class TransformScaleExample final: public TransformGizmoExample<ScaleGizmo>
{
public:
    TransformScaleExample(): TransformGizmoExample("Transform Scale") {}
};

VISUTWIN_EXAMPLE_MAIN(TransformScaleExample)
