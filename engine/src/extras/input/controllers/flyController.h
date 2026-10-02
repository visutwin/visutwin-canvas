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
        float rotateDamping = 0.98f;
        float moveDamping = 0.98f;

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
