// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include "elementComponent.h"
#include "elementComponentData.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    class ElementComponentSystem : public ComponentSystem<ElementComponent, ElementComponentData>
    {
    public:
        ElementComponentSystem(Engine* engine) : ComponentSystem(engine, "element") {}
    };
}
