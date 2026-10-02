// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "orbitController.h"

#include "core/math/quaternion.h"
#include "extras/input/math.h"

namespace visutwin::canvas
{
    namespace
    {
        Quaternion rotationOf(const Vector3& angles)
        {
            return Quaternion::fromEulerAngles(angles.getX(), angles.getY(), angles.getZ());
        }
    }

    void OrbitController::setPitchRange(const Vector2& range)
    {
        _targetRootPose.pitchRange = range;
        _rootPose.copy(_targetRootPose.rotate(Vector3(0.0f)));
    }

    void OrbitController::setYawRange(const Vector2& range)
    {
        _targetRootPose.yawRange = range;
        _rootPose.copy(_targetRootPose.rotate(Vector3(0.0f)));
    }

    void OrbitController::setZoomRange(const Vector2& range)
    {
        _targetChildPose.zRange = range;
        _childPose.copy(_targetChildPose.move(Vector3(0.0f)));
    }

    void OrbitController::attach(const Pose& pose, const bool smooth)
    {
        _targetRootPose.set(pose.getFocus(), pose.angles, 0.0f);
        _targetChildPose.position = Vector3(0.0f, 0.0f, pose.distance);

        if (!smooth) {
            _rootPose.copy(_targetRootPose);
            _childPose.copy(_targetChildPose);
        }
    }

    void OrbitController::detach()
    {
        _targetRootPose.copy(_rootPose);
        _targetChildPose.copy(_childPose);
    }

    const Pose& OrbitController::updatePose(InputFrame& frame, const float dt)
    {
        const InputValues values = frame.read();
        const auto& move = values.at("move");
        const auto& rotate = values.at("rotate");

        // move: pan the focus in the view plane, scale the distance
        const Vector3 offset = rotationOf(_rootPose.angles) * Vector3(move[0], move[1], 0.0f);
        _targetRootPose.move(offset);
        const float dist = _targetChildPose.position.getZ();
        _targetChildPose.move(Vector3(0.0f, 0.0f, dist * (1.0f + move[2]) - dist));

        // rotate
        _targetRootPose.rotate(Vector3(-rotate[1], -rotate[0], 0.0f));

        // smoothing
        _rootPose.lerp(_rootPose, _targetRootPose, damp(moveDamping, dt), damp(rotateDamping, dt), 1.0f);
        _childPose.lerp(_childPose, _targetChildPose, damp(zoomDamping, dt), 1.0f, 1.0f);

        // the final pose
        const Vector3 position = rotationOf(_rootPose.angles) * _childPose.position + _rootPose.position;
        _pose.set(position, _rootPose.angles, _childPose.position.getZ());
        return _pose;
    }
}
