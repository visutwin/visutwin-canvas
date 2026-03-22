// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "rigidBodyComponent.h"

#include "framework/components/collision/collisionComponent.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    RigidBodyComponent::RigidBodyComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    RigidBodyComponent::~RigidBodyComponent()
    {
        std::erase(_instances, this);
    }

    void RigidBodyComponent::setType(const std::string& type)
    {
        if (type == "dynamic") {
            _type = RigidBodyType::Dynamic;
        } else if (type == "kinematic") {
            _type = RigidBodyType::Kinematic;
        } else {
            _type = RigidBodyType::Static;
        }
    }

    CollisionComponent* RigidBodyComponent::collision() const
    {
        return entity() ? entity()->findComponent<CollisionComponent>() : nullptr;
    }
}
