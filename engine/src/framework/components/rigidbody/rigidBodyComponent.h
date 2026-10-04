// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "core/math/vector3.h"
#include "framework/components/component.h"

namespace visutwin::canvas
{
    class CollisionComponent;
    class PhysicsBody;
    class PhysicsWorld;

    enum class RigidBodyType
    {
        Static,
        Dynamic,
        Kinematic
    };

    /**
     * A body in the simulation supplied through `AppOptions::physicsWorld`.
     *
     * The component holds the settings; the body itself is created lazily on the
     * first update, from this component's values and the sibling
     * CollisionComponent's shape. Change a setting before that and it is simply
     * part of the description, so authoring order does not matter. After it, mass,
     * friction, restitution and damping update the live body in place (a body that
     * cannot take a new mass in place is rebuilt with its velocities carried over);
     * the type rebuilds it.
     *
     * Velocities, impulses and forces given before the body exists are held and
     * applied when it is created, so a body spawned and launched in the same frame
     * is launched. A force still applies over one step only: the first.
     *
     * With no world supplied nothing is created and nothing moves — the component
     * is inert but still answers `type()` and friends, which is what the
     * raycast-only behaviour that predates the seam relied on.
     *
     * Only an ACTIVE body is in the world: disabling this component, its entity (or a
     * parent), or the sibling CollisionComponent destroys the body at once, so nothing
     * collides with it, world raycasts miss it and the joints on it drop their
     * constraints. Enabled again, it is recreated on the next simulation sync at the
     * entity's current transform, at rest.
     */
    class RigidBodyComponent : public Component
    {
    public:
        /// Enabled before, and disabled after, every sibling component (order -1): the collision body must
        /// exist before anything can move or raycast against it.
        [[nodiscard]] int order() const override { return -1; }

        RigidBodyComponent(IComponentSystem* system, Entity* entity);
        ~RigidBodyComponent() override;

        void initializeComponentData() override {}
        void cloneFrom(const Component* source) override;
        void onDisable() override;


        RigidBodyType type() const { return _type; }
        void setType(RigidBodyType type);
        void setType(const std::string& type);

        /// Kilograms. Ignored for static and kinematic bodies.
        float mass() const { return _mass; }
        void setMass(float value);

        float friction() const { return _friction; }
        void setFriction(float value);

        /// 0 = no bounce, 1 = perfectly elastic.
        float restitution() const { return _restitution; }
        void setRestitution(float value);

        float linearDamping() const { return _linearDamping; }
        void setLinearDamping(float value);
        float angularDamping() const { return _angularDamping; }
        void setAngularDamping(float value);

        /// The body's velocity; before the body exists, the velocity it will be given.
        Vector3 linearVelocity() const;
        void setLinearVelocity(const Vector3& value);
        Vector3 angularVelocity() const;
        void setAngularVelocity(const Vector3& value);

        /// Continuous push in newtons, applied over the next step only.
        void applyForce(const Vector3& force);
        /// Instantaneous change of momentum, in newton-seconds.
        void applyImpulse(const Vector3& impulse);
        void applyTorque(const Vector3& torque);
        /// Instantaneous change of angular momentum, in newton-metre-seconds.
        void applyTorqueImpulse(const Vector3& impulse);

        /// Move the body outright and stop it. Use this rather than setting the
        /// entity's transform, which the simulation would overwrite on the next
        /// step.
        void teleport(const Vector3& position);

        /// Wake a body the solver has put to sleep.
        void activate();
        [[nodiscard]] bool isActive() const;

        CollisionComponent* collision() const;

        /// The simulated body, or null before the first update has created it. A body
        /// marked for rebuilding is still returned until the rebuild replaces it.
        /// JointComponent needs this to name the ends of a constraint.
        [[nodiscard]] PhysicsBody* physicsBody() const { return _body; }

        /// Called by RigidBodyComponentSystem; creates the body if it does not
        /// exist yet and mirrors its transform onto the entity. A body that is not
        /// simulated (see simulated()) is taken out of the world instead.
        void syncFromSimulation(PhysicsWorld& world);

        /// True when the body belongs in the world: this component is active and the
        /// sibling CollisionComponent, if there is one, is active too.
        [[nodiscard]] bool simulated() const;

        /// Destroys the body, if one exists, and forgets anything held for it; joints on
        /// it let go first. The next sync of a simulated body creates a new one at the
        /// entity's transform with no velocity. Called when this component, its entity
        /// or its collision component is disabled.
        void removeFromSimulation();

    private:
        // Writes a dynamic body's pose to an entity with a negative local scale or a
        // mirrored world transform.
        void setMirroredTransform(const Vector3& position, const Quaternion& bodyRotation);

    public:
        /// Called by RigidBodyComponentSystem before the world is destroyed.
        void releaseBody(PhysicsWorld& world);

    private:
        void markBodyStale() { _bodyStale = true; }

        // The body calls should reach: null before creation and while a rebuild is
        // pending, when they are held for the new body instead.
        [[nodiscard]] PhysicsBody* liveBody() const { return _bodyStale ? nullptr : _body; }
        void applyPendingTo(PhysicsBody& body);
        void clearPending();


        RigidBodyType _type = RigidBodyType::Static;
        float _mass = 1.0f;
        float _friction = 0.5f;
        float _restitution = 0.0f;
        float _linearDamping = 0.0f;
        float _angularDamping = 0.0f;

        // Held for a body that does not exist yet. A velocity set replaces any impulse
        // held before it, as it would have overwritten the velocity that impulse gave.
        Vector3 _pendingLinearVelocity = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 _pendingAngularVelocity = Vector3(0.0f, 0.0f, 0.0f);
        bool _hasPendingLinearVelocity = false;
        bool _hasPendingAngularVelocity = false;
        Vector3 _pendingImpulse = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 _pendingTorqueImpulse = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 _pendingForce = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 _pendingTorque = Vector3(0.0f, 0.0f, 0.0f);
        bool _hasPendingPushes = false;

        PhysicsBody* _body = nullptr;
        PhysicsWorld* _world = nullptr;
        bool _bodyStale = false;
    };
}
