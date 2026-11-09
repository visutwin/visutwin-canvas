// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#pragma once

#include "extras/input/input.h"

namespace visutwin::canvas
{
    /**
     * The pose orbits a focus point at a distance: `rotate` deltas turn the view around
     * the focus within yawRange and pitchRange, the first two `move` components pan the
     * focus point, and the third scales the distance within zoomRange. Motion is smoothed
     * with rotateDamping, moveDamping and zoomDamping (0 = none, 1 = full). With
     * horizontalPan, the pan moves the focus point across the ground instead of the plane
     * of the view.
     */
    class OrbitController : public InputController
    {
    public:
        // DEVIATION: 0.9 where upstream uses 0.98. Smoothing time constant ~9.5 ms (settles in
        // ~45 ms after the pointer stops) instead of ~50 ms (~230 ms), so the camera follows the
        // pointer instead of trailing it. damp() reads these per millisecond.
        float rotateDamping = 0.9f;
        float moveDamping = 0.9f;
        float zoomDamping = 0.9f;

        /// Whether the pan moves the focus point across the horizontal plane, as when dragging
        /// a map, instead of across the plane of the view. The first two `move` components are
        /// then the distances to move the focus point to the right and forward, level with the
        /// ground, so the height of the focus point does not change.
        bool horizontalPan = false;

        void setPitchRange(const Vector2& range);
        [[nodiscard]] const Vector2& pitchRange() const { return _targetRootPose.pitchRange; }
        void setYawRange(const Vector2& range);
        [[nodiscard]] const Vector2& yawRange() const { return _targetRootPose.yawRange; }
        void setZoomRange(const Vector2& range);
        [[nodiscard]] const Vector2& zoomRange() const { return _targetChildPose.zRange; }

        void attach(const Pose& pose, bool smooth = true) override;
        void detach() override;
        const Pose& updatePose(InputFrame& frame, float dt) override;

    private:
        Pose _targetRootPose;
        Pose _rootPose;
        Pose _targetChildPose;
        Pose _childPose;
    };
}
