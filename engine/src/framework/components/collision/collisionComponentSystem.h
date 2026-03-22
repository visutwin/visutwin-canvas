// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include "collisionComponent.h"
#include "collisionComponentData.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    class CollisionComponentSystem : public ComponentSystem<CollisionComponent, CollisionComponentData>
    {
    public:
        CollisionComponentSystem(Engine* engine) : ComponentSystem(engine, "collision") {}
    };
}
