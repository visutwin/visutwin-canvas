// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.07.2026
//
#pragma once

#include "animComponent.h"
#include "animComponentData.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    class AnimComponentSystem : public ComponentSystem<AnimComponent, AnimComponentData>
    {
    public:
        explicit AnimComponentSystem(Engine* engine);

        ~AnimComponentSystem() override;

        /// Animation updates run so far; components with an evaluation interval are scheduled on it.
        uint64_t updateIndex() const { return _updateIndex; }

        /// The last update's components that evaluated their animation, that skipped it as not
        /// visible, and that skipped it under their evaluation interval (also in FrameStats).
        int evaluatedCount() const { return _evaluated; }
        int offscreenCount() const { return _offscreen; }
        int intervalSkippedCount() const { return _intervalSkipped; }

    private:
        void onAnimationUpdate(float dt);

        uint64_t _updateIndex = 0;
        int _evaluated = 0;
        int _offscreen = 0;
        int _intervalSkipped = 0;
    };
}
