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


    enum class PhysicsJointType
    {
        Fixed,   ///< welds two bodies; no relative motion
        Ball,    ///< shared point, rotation limited by a swing/twist cone
        Hinge,   ///< one rotation axis, optionally limited and motorised
        Slider,  ///< one translation axis, optionally limited and motorised
        SixDof   ///< per-axis motion with optional springs
    };

    /// One constraint, described in the JOINT FRAME rather than as a pair of
    /// anchors: `framePosition` / `frameRotation` are the world pose of the joint
    /// itself, and its local **X axis is the primary axis** — the hinge's rotation
    /// axis, the slider's travel axis, the ball joint's twist axis. That is
    /// upstream's convention, and it is why a joint lives on its own entity there:
    /// the entity's transform IS the frame.
    struct PhysicsJointDesc
    {
        PhysicsJointType type = PhysicsJointType::Ball;

        /// End A. Required.
        PhysicsBody* bodyA = nullptr;
        /// End B. Null anchors that end to the world.
        PhysicsBody* bodyB = nullptr;

        Vector3 framePosition = Vector3(0.0f, 0.0f, 0.0f);
        Quaternion frameRotation = Quaternion();

        /// Hinge angle in RADIANS or slider offset in metres, about/along the
        /// primary axis.
        bool enableLimits = false;
        float minLimit = 0.0f;
        float maxLimit = 0.0f;

        /// Hinge (radians/second) or slider (metres/second). The motor is only
        /// active when `maxMotorForce` is above zero, matching upstream.
        float motorSpeed = 0.0f;
        float maxMotorForce = 0.0f;

        /// Ball joint cone, in radians. Y and Z are the two swing half-angles
        /// about the axes perpendicular to the primary one.
        float swingLimitY = 0.0f;
        float swingLimitZ = 0.0f;
        float twistLimit = 0.0f;

        /// Above zero, the joint breaks once the impulse it carries exceeds this,
        /// and reports it through `isBroken()`.
        float breakImpulse = 0.0f;

        /// SixDof only: which linear axes are free, and an optional spring on each.
        bool linearFree[3] = {false, false, false};
        Vector3 linearStiffness = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 linearEquilibrium = Vector3(0.0f, 0.0f, 0.0f);
    };

    /// One constraint. Owned by the world; valid until destroyJoint.
    class PhysicsJoint
    {
    public:
        virtual ~PhysicsJoint() = default;

        /// A disabled joint stays allocated but stops constraining.
        virtual void setEnabled(bool enabled) = 0;
        [[nodiscard]] virtual bool enabled() const = 0;

        /// Hinge and slider only. Takes effect on the next step.
        virtual void setMotorSpeed(float speed) = 0;
        [[nodiscard]] virtual float motorSpeed() const = 0;

        /// True once the joint has carried more impulse than its break threshold.
        /// A broken joint disables itself and stays broken.
        [[nodiscard]] virtual bool isBroken() const = 0;

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
        /// Destroys the body AND any joint still attached to it — a constraint
        /// outliving one of its ends is a dangling reference inside the solver.
        virtual void destroyBody(PhysicsBody* body) = 0;

        /// The returned joint is owned by the world. Returns null when the
        /// description names no real body at either end.
        virtual PhysicsJoint* createJoint(const PhysicsJointDesc& desc) = 0;
        virtual void destroyJoint(PhysicsJoint* joint) = 0;

        /// Nearest hit along the segment, or nothing.
        [[nodiscard]] virtual std::optional<PhysicsRaycastHit> raycastFirst(
            const Vector3& start, const Vector3& end) const = 0;
        /// Every hit along the segment, nearest first.
        [[nodiscard]] virtual std::vector<PhysicsRaycastHit> raycastAll(
            const Vector3& start, const Vector3& end) const = 0;
    };
}
