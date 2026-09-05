// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The physics backend seam (upstream PhysicsWorld / PhysicsBody).
//
// The engine owns no simulation. An application supplies a PhysicsWorld through
// AppOptions, exactly the way it supplies component systems, and
// RigidBodyComponentSystem drives whatever it is given. With no world supplied
// the rigid-body component keeps its previous behaviour: it holds settings and
// answers raycasts from a CPU sweep over collision bounds, and nothing moves.
//
// A Jolt-backed implementation ships in `jolt/joltPhysicsWorld.h` when the engine
// is built with VISUTWIN_PHYSICS_JOLT.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/math/quaternion.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    class Entity;

    enum class PhysicsShapeType
    {
        Box,
        Sphere,
        Capsule,
        Cylinder,
        Plane
    };

    enum class PhysicsMotionType
    {
        Static,     ///< never moves; cheapest, and the default
        Dynamic,    ///< moved by the simulation
        Kinematic   ///< moved by the application, pushes dynamic bodies
    };

    /// Everything the backend needs to create one body. Shape fields are read
    /// according to `shape`: Box uses halfExtents, Sphere uses radius, Capsule and
    /// Cylinder use radius and height (height is the FULL height, cylinder part
    /// included for a capsule, matching CollisionComponent).
    struct PhysicsBodyDesc
    {
        PhysicsShapeType shape = PhysicsShapeType::Box;
        PhysicsMotionType motion = PhysicsMotionType::Static;

        Vector3 halfExtents = Vector3(0.5f, 0.5f, 0.5f);
        float radius = 0.5f;
        float height = 1.0f;

        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
        Quaternion rotation = Quaternion();

        float mass = 1.0f;
        float friction = 0.5f;
        float restitution = 0.0f;
        float linearDamping = 0.0f;
        float angularDamping = 0.0f;

        /// Owner, so a raycast can report which entity it hit. Never dereferenced
        /// by the backend.
        Entity* entity = nullptr;
    };

    /// One simulated body. Created and destroyed through PhysicsWorld; the handle
    /// stays valid until PhysicsWorld::destroyBody is called on it.
    class PhysicsBody
    {
    public:
        virtual ~PhysicsBody() = default;

        virtual Vector3 position() const = 0;
        virtual Quaternion rotation() const = 0;

        /// Move the body outright, ignoring the simulation. For a dynamic body this
        /// is a teleport and clears nothing, so callers usually want to zero the
        /// velocities as well.
        virtual void setTransform(const Vector3& position, const Quaternion& rotation) = 0;

        virtual Vector3 linearVelocity() const = 0;
        virtual void setLinearVelocity(const Vector3& value) = 0;
        virtual Vector3 angularVelocity() const = 0;
        virtual void setAngularVelocity(const Vector3& value) = 0;

        /// Continuous push, in newtons; applied over the next step only.
        virtual void applyForce(const Vector3& force) = 0;
        /// Instantaneous change of momentum, in newton-seconds.
        virtual void applyImpulse(const Vector3& impulse) = 0;
        virtual void applyTorque(const Vector3& torque) = 0;

        /// A body the solver has put to sleep costs nothing until something touches
        /// it. Setting a velocity or applying an impulse wakes it; moving it with
        /// setTransform does not, so do this after a teleport.
        virtual void activate() = 0;
        [[nodiscard]] virtual bool isActive() const = 0;
    };

    struct PhysicsRaycastHit
    {
        Entity* entity = nullptr;
        Vector3 point = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
        float fraction = 0.0f;
    };

    class PhysicsWorld
    {
    public:
        virtual ~PhysicsWorld() = default;

        /// Advance the simulation. `dt` is the frame's delta in seconds; a backend
        /// is free to substep internally.
        virtual void step(float dt) = 0;

        virtual void setGravity(const Vector3& gravity) = 0;
        [[nodiscard]] virtual Vector3 gravity() const = 0;

        /// The returned body is owned by the world. Returns null when the
        /// description cannot be realised (a zero-extent shape, say).
        virtual PhysicsBody* createBody(const PhysicsBodyDesc& desc) = 0;
        virtual void destroyBody(PhysicsBody* body) = 0;

        /// Nearest hit along the segment, or nothing.
        [[nodiscard]] virtual std::optional<PhysicsRaycastHit> raycastFirst(
            const Vector3& start, const Vector3& end) const = 0;
        /// Every hit along the segment, nearest first.
        [[nodiscard]] virtual std::vector<PhysicsRaycastHit> raycastAll(
            const Vector3& start, const Vector3& end) const = 0;
    };
}
