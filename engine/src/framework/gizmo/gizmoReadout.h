// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// A text label a gizmo shows this frame, for the application to draw.
//
// DEVIATION: upstream shows each as an HTML element over the canvas; here the gizmo only keeps
// what to show and where, and the application draws it (the examples with a UI text element,
// see examples/gizmoLabels.h).
//
#pragma once

#include <string>

namespace visutwin::canvas
{
    struct GizmoReadout
    {
        bool visible = false;
        /// The text, labels in their colour as text markup (`[color="#rrggbb"]X[/color]`).
        std::string markup;
        /// Where it goes, in canvas points from the top-left: the horizontal centre of the text
        /// and its top edge.
        float x = 0.0f;
        float y = 0.0f;
    };

    /// The gap between the point a label is placed below and its top edge, in canvas points.
    constexpr float kGizmoReadoutGap = 6.0f;
}
