// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include "gsplatComponent.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    struct GSplatComponentData
    {
        bool enabled = true;
    };

    class GSplatComponentSystem : public ComponentSystem<GSplatComponent, GSplatComponentData>
    {
    public:
        explicit GSplatComponentSystem(Engine* engine)
            : ComponentSystem(engine, "gsplat")
        {
        }
    };
}
