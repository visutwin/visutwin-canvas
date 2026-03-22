// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include "animationComponent.h"
#include "animationComponentData.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    class AnimationComponentSystem : public ComponentSystem<AnimationComponent, AnimationComponentData>
    {
    public:
        explicit AnimationComponentSystem(Engine* engine);

        ~AnimationComponentSystem() override;

    private:
        void onAnimationUpdate(float dt);
    };
}
