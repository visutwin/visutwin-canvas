// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "framework/components/component.h"

namespace visutwin::canvas
{
    class CollisionComponent;

    enum class RigidBodyType
    {
        Static,
        Dynamic,
        Kinematic
    };

    class RigidBodyComponent : public Component
    {
    public:
        RigidBodyComponent(IComponentSystem* system, Entity* entity);
        ~RigidBodyComponent() override;

        void initializeComponentData() override {}

        static const std::vector<RigidBodyComponent*>& instances() { return _instances; }

        RigidBodyType type() const { return _type; }
        void setType(const RigidBodyType type) { _type = type; }
        void setType(const std::string& type);

        CollisionComponent* collision() const;

    private:
        inline static std::vector<RigidBodyComponent*> _instances;
        RigidBodyType _type = RigidBodyType::Static;
    };
}
