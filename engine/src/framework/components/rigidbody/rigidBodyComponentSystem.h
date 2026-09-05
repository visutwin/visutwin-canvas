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
    class PhysicsWorld;
    struct RaycastResult
    {
        Entity* entity = nullptr;
        CollisionComponent* collision = nullptr;
        RigidBodyComponent* rigidbody = nullptr;
        Vector3 point = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
        float hitFraction = 0.0f;
    };

    /**
     * Drives whatever `AppOptions::physicsWorld` supplied: steps it once per
     * update, then mirrors body transforms onto entities.
     *
     * With no world supplied the system still exists and still answers raycasts,
     * from a CPU sweep over collision bounds. That is the behaviour that predates
     * the physics seam, and it is what a scene that only needs picking wants.
     */
    class RigidBodyComponentSystem : public ComponentSystem<RigidBodyComponent, RigidBodyComponentData>
    {
    public:
        explicit RigidBodyComponentSystem(Engine* engine);
        ~RigidBodyComponentSystem() override;

        /// Nearest hit along the segment. Goes to the physics world when there is
        /// one, and to the CPU bounds sweep otherwise.
        std::optional<RaycastResult> raycastFirst(const Vector3& start, const Vector3& end) const;
        std::vector<RaycastResult> raycastAll(const Vector3& start, const Vector3& end) const;

        /// The world this system is driving, or null.
        [[nodiscard]] PhysicsWorld* world() const { return _world; }

    private:
        std::optional<RaycastResult> raycastFirstCpu(const Vector3& start, const Vector3& end) const;
        std::vector<RaycastResult> raycastAllCpu(const Vector3& start, const Vector3& end) const;

        PhysicsWorld* _world = nullptr;
    };
}
