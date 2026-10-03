// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2026
//
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
            // Inactive joints are visited too: the sync drops the constraint of a joint
            // that is no longer active.
            for (auto* joint : JointComponent::instances()) {
                if (joint && joint->entity()) {
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
