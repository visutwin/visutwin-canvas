// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include "buttonComponent.h"
#include "buttonComponentData.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    class ButtonComponentSystem : public ComponentSystem<ButtonComponent, ButtonComponentData>
    {
    public:
        ButtonComponentSystem(Engine* engine) : ComponentSystem(engine, "button") {}
    };
}
