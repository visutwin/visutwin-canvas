// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include "lightComponent.h"
#include "lightComponentData.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    class LightComponentSystem : public ComponentSystem<LightComponent, LightComponentData>
    {
    public:
        LightComponentSystem(Engine* engine) : ComponentSystem(engine, "light") {}
    };
}
