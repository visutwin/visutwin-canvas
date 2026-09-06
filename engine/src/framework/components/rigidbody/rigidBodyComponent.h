// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
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
     * part of the description; change one after and the component recreates the
     * body, so authoring order does not matter.
     *
     * With no world supplied nothing is created and nothing moves — the component
     * is inert but still answers `type()` and friends, which is what the
     * raycast-only behaviour that predates the seam relied on.
     */
    class RigidBodyComponent : public Component
    {
    public:
        /// Enabled before, and disabled after, every sibling component (upstream
        /// gives RigidBodyComponent `static order = -1`): the collision body must
        /// exist before anything can move or raycast against it.
        [[nodiscard]] int order() const override { return -1; }

        RigidBodyComponent(IComponentSystem* system, Entity* entity);
        ~RigidBodyComponent() override;

        void initializeComponentData() override {}

        static const std::vector<RigidBodyComponent*>& instances() { return _instances; }

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

        Vector3 linearVelocity() const;
        void setLinearVelocity(const Vector3& value);
        Vector3 angularVelocity() const;
        void setAngularVelocity(const Vector3& value);

        /// Continuous push in newtons, applied over the next step only.
        void applyForce(const Vector3& force);
        /// Instantaneous change of momentum, in newton-seconds.
        void applyImpulse(const Vector3& impulse);
        void applyTorque(const Vector3& torque);

        /// Move the body outright and stop it. Use this rather than setting the
        /// entity's transform, which the simulation would overwrite on the next
        /// step.
        void teleport(const Vector3& position);

        /// Wake a body the solver has put to sleep.
        void activate();
        [[nodiscard]] bool isActive() const;

        CollisionComponent* collision() const;

        /// The simulated body, or null before the first update has created it.
        /// JointComponent needs this to name the ends of a constraint.
        [[nodiscard]] PhysicsBody* physicsBody() const { return _body; }

        /// Called by RigidBodyComponentSystem; creates the body if it does not
        /// exist yet and mirrors its transform onto the entity.
        void syncFromSimulation(PhysicsWorld& world);
        /// Called by RigidBodyComponentSystem before the world is destroyed.
        void releaseBody(PhysicsWorld& world);

    private:
        void markBodyStale() { _bodyStale = true; }

        inline static std::vector<RigidBodyComponent*> _instances;

        RigidBodyType _type = RigidBodyType::Static;
        float _mass = 1.0f;
        float _friction = 0.5f;
        float _restitution = 0.0f;
        float _linearDamping = 0.0f;
        float _angularDamping = 0.0f;

        PhysicsBody* _body = nullptr;
        PhysicsWorld* _world = nullptr;
        bool _bodyStale = false;
    };
}
