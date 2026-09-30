// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include "layoutChildComponent.h"
#include "layoutChildComponentData.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    class LayoutChildComponentSystem : public ComponentSystem<LayoutChildComponent, LayoutChildComponentData>
    {
    public:
        explicit LayoutChildComponentSystem(Engine* engine) : ComponentSystem(engine, "layoutchild") {}
    };
}
