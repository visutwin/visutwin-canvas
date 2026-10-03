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
     * Free flight: `rotate` deltas turn the pose about yaw and pitch (within yawRange and
     * pitchRange) and `move` deltas translate it along its own right, up and forward
     * axes. Smoothed with rotateDamping and moveDamping.
     */
    class FlyController : public InputController
    {
    public:
        // DEVIATION: 0.9 where upstream uses 0.98. Smoothing time constant ~9.5 ms (settles in
        // ~45 ms after the pointer stops) instead of ~50 ms (~230 ms), so the camera follows the
        // pointer instead of trailing it. damp() reads these per millisecond.
        float rotateDamping = 0.9f;
        float moveDamping = 0.9f;

        void setPitchRange(const Vector2& range);
        [[nodiscard]] const Vector2& pitchRange() const { return _targetPose.pitchRange; }
        void setYawRange(const Vector2& range);
        [[nodiscard]] const Vector2& yawRange() const { return _targetPose.yawRange; }

        void attach(const Pose& pose, bool smooth = true) override;
        void detach() override;
        const Pose& updatePose(InputFrame& frame, float dt) override;

    private:
        Pose _targetPose;
    };
}
