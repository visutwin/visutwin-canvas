// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include "renderComponent.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    /**
    * Allows an Entity to render a mesh or a primitive shape like a box, capsule, sphere, cylinder, cone etc
     */
    class RenderComponentSystem : public ComponentSystem<RenderComponent, RenderComponentData>
    {
    public:
        RenderComponentSystem(Engine* engine) : ComponentSystem(engine, "render") {}
    };
}
