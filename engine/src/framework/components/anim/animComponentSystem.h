// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
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

    private:
        void onAnimationUpdate(float dt);
    };
}
