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
     * Drives whatever `AppOptions::physicsWorld` supplied: steps it on the engine's
     * FIXED update, then mirrors body transforms onto entities.
     *
     * Fixed rather than per-frame: a simulation advanced by the frame delta gives a
     * different trajectory at every frame rate, and a long frame integrates one big
     * step that tunnels through thin geometry. Engine::update owns the accumulator
     * (see Engine::setFixedDeltaTime) and calls this zero or more times per frame.
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

        /// Every hit along the segment, NEAREST FIRST. Both the physics-world path and
        /// the CPU fallback sort, so the order does not depend on whether a world was
        /// supplied — it used to, which made the order a configuration detail. (Upstream
        /// leaves them unordered unless asked; a single contract is cheaper to reason
        /// about than a flag, and the caller that wants raw order can stop sorting here.)
        std::vector<RaycastResult> raycastAll(const Vector3& start, const Vector3& end) const;

        /// Advance the simulation by `dt` seconds and write body transforms back to
        /// their entities. Called for you on each fixed update with the fixed delta
        /// scaled by timeScale(); call it directly only to drive the simulation from
        /// some other clock, and set timeScale(0) first so it is not also stepped
        /// automatically. Does nothing when no world was supplied.
        void step(float dt);

        /// Scales the fixed delta the simulation is advanced by. 1 is real time, 0.25
        /// quarter speed. ZERO PAUSES the simulation: nothing is stepped and no
        /// transform is written back, while the rest of the engine keeps running.
        /// Negative values are treated as zero.
        [[nodiscard]] float timeScale() const { return _timeScale; }
        void setTimeScale(const float scale) { _timeScale = scale; }

        /// The world this system is driving, or null.
        [[nodiscard]] PhysicsWorld* world() const { return _world; }

    private:
        std::optional<RaycastResult> raycastFirstCpu(const Vector3& start, const Vector3& end) const;
        std::vector<RaycastResult> raycastAllCpu(const Vector3& start, const Vector3& end) const;

        /// Lazily resolved: a component system may be constructed before the engine has
        /// stored everything AppOptions carried, and a world read once at construction
        /// would be null forever — a frozen scene with nothing to show why.
        PhysicsWorld* resolveWorld();

        PhysicsWorld* _world = nullptr;
        float _timeScale = 1.0f;
    };
}
