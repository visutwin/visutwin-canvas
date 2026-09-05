// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include "framework/components/componentSystem.h"
#include "jointComponent.h"
#include "jointComponentData.h"

namespace visutwin::canvas
{
    class PhysicsWorld;

    /**
     * Creates the constraints its components describe, once both ends have a
     * physics body.
     *
     * Register it AFTER RigidBodyComponentSystem so bodies exist by the time
     * joints are built; the joint component tolerates the other order too, it just
     * spends a frame waiting.
     */
    class JointComponentSystem : public ComponentSystem<JointComponent, JointComponentData>
    {
    public:
        explicit JointComponentSystem(Engine* engine);
        ~JointComponentSystem() override;

    private:
        PhysicsWorld* _world = nullptr;
    };
}
