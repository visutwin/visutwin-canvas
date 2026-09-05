// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "jointComponentSystem.h"

#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/physics/physicsWorld.h"

namespace visutwin::canvas
{
    JointComponentSystem::JointComponentSystem(Engine* engine)
        : ComponentSystem(engine, "joint")
    {
        if (engine == nullptr || engine->systems() == nullptr) {
            return;
        }

        engine->systems()->on("update", [this](float) {
            // Resolved lazily for the same reason RigidBodyComponentSystem does:
            // a null world read once at construction leaves the subsystem silently
            // inert.
            if (_world == nullptr) {
                _world = _engine ? _engine->physicsWorld() : nullptr;
            }
            if (_world == nullptr) {
                return;
            }
            for (auto* joint : JointComponent::instances()) {
                if (joint && joint->enabled() && joint->entity() && joint->entity()->enabled()) {
                    joint->syncToSimulation(*_world);
                }
            }
        }, this);
    }

    JointComponentSystem::~JointComponentSystem()
    {
        if (_engine && _engine->systems()) {
            _engine->systems()->off("update", HandleEventCallback(), this);
        }
        if (_world != nullptr) {
            for (auto* joint : JointComponent::instances()) {
                if (joint) { joint->releaseJoint(*_world); }
            }
        }
    }
}
