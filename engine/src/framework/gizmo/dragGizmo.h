// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// The base of the gizmos dragged to edit values: the transform gizmos and the handle gizmos.
// With `snap` on, lengths snap to `snapIncrement` and angles to `snapAngleIncrement`; it also
// holds the drag maths and the readout label markup the two families share.
//
#pragma once

#include <string>

#include "core/math/color.h"
#include "framework/gizmo/gizmo.h"

namespace visutwin::canvas
{
    class DragGizmo : public Gizmo
    {
    public:
        DragGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer, const std::string& name = "gizmo:drag");

        /// Whether dragging snaps to the increments.
        bool snap = false;
        /// The increment lengths and scale factors snap to.
        float snapIncrement = 1.0f;
        /// The increment angles snap to, in degrees.
        float snapAngleIncrement = 5.0f;

        /// `text` in `color`, as text markup (`[color="#rrggbb"]text[/color]`).
        static std::string labelMarkup(const std::string& text, const Color& color);

    protected:
        /// `value` rounded to a multiple of `increment` while snapping.
        float snapValue(float value, float increment) const;
        /// Each component rounded to a multiple of `increment` while snapping.
        Vector3 snapVector(const Vector3& value, float increment) const;
        /// A length rounded to a multiple of `snapIncrement` while snapping, at least one
        /// increment, so a size never snaps to zero.
        float snapLength(float value) const;
        /// An angle in degrees rounded to a multiple of `snapAngleIncrement` while snapping.
        float snapAngle(float value) const;

        /// The unit normal of the plane through a line (unit direction `dir`) that best faces
        /// the camera (unit direction `facing`), for dragging along the line. False when the
        /// line points at the camera.
        static bool lineNormal(const Vector3& dir, const Vector3& facing, Vector3& out);
    };
}
