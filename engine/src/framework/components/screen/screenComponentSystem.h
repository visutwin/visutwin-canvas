// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include "framework/components/componentSystem.h"
#include "screenComponent.h"
#include "screenComponentData.h"

namespace visutwin::canvas
{
    class ScreenComponentSystem : public ComponentSystem<ScreenComponent, ScreenComponentData>
    {
    public:
        ScreenComponentSystem(Engine* engine) : ComponentSystem(engine, "screen") {}
    };
}
