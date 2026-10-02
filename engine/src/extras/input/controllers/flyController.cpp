// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "flyController.h"

#include "core/math/quaternion.h"
#include "extras/input/math.h"

namespace visutwin::canvas
{
    void FlyController::setPitchRange(const Vector2& range)
    {
        _targetPose.pitchRange = range;
        _pose.copy(_targetPose.rotate(Vector3(0.0f)));
    }

    void FlyController::setYawRange(const Vector2& range)
    {
        _targetPose.yawRange = range;
        _pose.copy(_targetPose.rotate(Vector3(0.0f)));
    }

    void FlyController::attach(const Pose& pose, const bool smooth)
    {
        _targetPose.copy(pose);
        if (!smooth) {
            _pose.copy(_targetPose);
        }
    }

    void FlyController::detach()
    {
        _targetPose.copy(_pose);
    }

    const Pose& FlyController::updatePose(InputFrame& frame, const float dt)
    {
        const InputValues values = frame.read();
        const auto& move = values.at("move");
        const auto& rotate = values.at("rotate");

        // rotate
        _targetPose.rotate(Vector3(-rotate[1], -rotate[0], 0.0f));

        // move along the pose's own axes
        const Quaternion rotation = Quaternion::fromEulerAngles(
            _pose.angles.getX(), _pose.angles.getY(), _pose.angles.getZ());
        const Vector3 forward = rotation * Vector3(0.0f, 0.0f, -1.0f);
        const Vector3 right = rotation * Vector3(1.0f, 0.0f, 0.0f);
        const Vector3 up = rotation * Vector3(0.0f, 1.0f, 0.0f);
        _targetPose.move(forward * move[2] + right * move[0] + up * move[1]);

        // smoothing
        _pose.lerp(_pose, _targetPose, damp(moveDamping, dt), damp(rotateDamping, dt), damp(moveDamping, dt));
        return _pose;
    }
}
