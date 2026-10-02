// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.09.2026
//
// Port of upstream gizmos/transform-translate.
//
// A default white box at the origin, lit by a default directional light at euler
// (0, 0, -60) with ambient 0.2, over a 4x4 grid. A translate gizmo, drawn in its own
// depth-cleared layer from Gizmo::createLayer, is attached to the box; its size is
// 1024 / the canvas height, kept up to date as the window resizes, and the orbit camera
// (focused on the origin, zoom 2..10, pitch +/-89.999) stops responding while the gizmo
// holds the pointer (the 'gizmo:pointer' app event).
//
// DEVIATIONS:
// - No controls panel: upstream's initial values apply (snap off, world space, the
//   default theme, drag mode 'selected', perspective at 45 degrees).
// - Upstream's Grid script is a pristine-grid shader on a blended plane; grid.h draws
//   it with wide lines instead, over the scaled (4, 1, 4) extent.
// - The example harness re-enables the camera controls at the top of every frame (for
//   its HUD), so the gizmo's pointer hold is re-applied in update().
//
// The scene is the one all three transform gizmo examples share (transformGizmoExample.h);
// this file chooses the gizmo.
//
#include "../transformGizmoExample.h"
#include "framework/gizmo/translateGizmo.h"

using namespace visutwin::canvas;

class TransformTranslateExample final: public TransformGizmoExample<TranslateGizmo>
{
public:
    TransformTranslateExample(): TransformGizmoExample("Transform Translate") {}
};

VISUTWIN_EXAMPLE_MAIN(TransformTranslateExample)
