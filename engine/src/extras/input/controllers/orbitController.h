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
     * with rotateDamping, moveDamping and zoomDamping (0 = none, 1 = full).
     */
    class OrbitController : public InputController
    {
    public:
        float rotateDamping = 0.98f;
        float moveDamping = 0.98f;
        float zoomDamping = 0.98f;

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
