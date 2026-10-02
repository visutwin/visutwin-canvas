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
     * Ignores its input and eases the pose from where it was toward the pose given to
     * attach(), smoothed by focusDamping; complete() says when it has arrived. Used to
     * glide a camera onto a new target before handing over to another controller.
     */
    class FocusController : public InputController
    {
    public:
        float focusDamping = 0.98f;

        void attach(const Pose& pose, bool smooth = true) override;
        void detach() override;
        [[nodiscard]] bool complete() const;
        const Pose& updatePose(InputFrame& frame, float dt) override;

    private:
        Pose _targetRootPose;
        Pose _rootPose;
        Pose _targetChildPose;
        Pose _childPose;
    };
}
