// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <optional>
#include <vector>

#include "core/math/vector3.h"
#include "framework/components/componentSystem.h"
#include "rigidBodyComponent.h"
#include "rigidBodyComponentData.h"

namespace visutwin::canvas
{
    struct RaycastResult
    {
        Entity* entity = nullptr;
        CollisionComponent* collision = nullptr;
        RigidBodyComponent* rigidbody = nullptr;
        Vector3 point = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
        float hitFraction = 0.0f;
    };

    class RigidBodyComponentSystem : public ComponentSystem<RigidBodyComponent, RigidBodyComponentData>
    {
    public:
        RigidBodyComponentSystem(Engine* engine) : ComponentSystem(engine, "rigidbody") {}

        std::optional<RaycastResult> raycastFirst(const Vector3& start, const Vector3& end) const;
        std::vector<RaycastResult> raycastAll(const Vector3& start, const Vector3& end) const;
    };
}
