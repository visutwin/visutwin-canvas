// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include "cameraComponent.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    /*
    * Used to add and remove CameraComponent from Entities. It also holds an array of all active cameras.
    */
    class CameraComponentSystem : public ComponentSystem<CameraComponent, CameraComponentData>
    {
    public:
        CameraComponentSystem(Engine* engine) : ComponentSystem(engine, "camera") {}

    protected:
        void initializeComponentData(CameraComponentSystem& component);
    };
}
