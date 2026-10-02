// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "focusController.h"

#include "core/math/quaternion.h"
#include "extras/input/math.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float kEpsilon = 0.001f;
    }

    void FocusController::attach(const Pose& pose, const bool smooth)
    {
        _targetRootPose.set(pose.getFocus(), pose.angles, 0.0f);
        _targetChildPose.position = Vector3(0.0f, 0.0f, pose.distance);

        if (!smooth) {
            _rootPose.copy(_targetRootPose);
            _childPose.copy(_targetChildPose);
        }
    }

    void FocusController::detach()
    {
        _targetRootPose.copy(_rootPose);
        _targetChildPose.copy(_childPose);
    }

    bool FocusController::complete() const
    {
        return _targetRootPose.equalsApprox(_rootPose, kEpsilon) &&
            _targetChildPose.equalsApprox(_childPose, kEpsilon);
    }

    const Pose& FocusController::updatePose(InputFrame& frame, const float dt)
    {
        // The input is not used.
        frame.read();

        _rootPose.lerp(_rootPose, _targetRootPose, damp(focusDamping, dt), damp(focusDamping, dt), 1.0f);
        _childPose.lerp(_childPose, _targetChildPose, damp(focusDamping, dt), 1.0f, 1.0f);

        const Vector3 position = Quaternion::fromEulerAngles(
            _rootPose.angles.getX(), _rootPose.angles.getY(), _rootPose.angles.getZ()) * _childPose.position
            + _rootPose.position;
        _pose.set(position, _rootPose.angles, _childPose.position.length());
        return _pose;
    }
}
