// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// What a RigidBodyComponent asks of its body, and when.
//
//  - The body is created lazily, on the first simulation sync. A velocity, impulse,
//    torque impulse, force or torque given before then is held and applied to the body
//    when it is created, before the step, so a body spawned and launched in one frame
//    is launched. The getters answer the held velocity meanwhile, and a velocity set
//    replaces an impulse held before it.
//  - Mass, friction, restitution and damping update the LIVE body: no rebuild, and an
//    unchanged value reaches nothing at all. A body that cannot take a new mass in place
//    is rebuilt with its velocities carried over.
//  - teleport() before the body exists stops it: nothing held is applied.
//
// The components run against a world of recording bodies, so every call is visible.

#include <iostream>
#include <memory>
#include <optional>
#include <vector>

#include "framework/components/component.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/entity.h"
#include "framework/physics/physicsWorld.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    class RecordingBody final : public PhysicsBody
    {
    public:
        explicit RecordingBody(const PhysicsBodyDesc& d)
            : desc(d), mass(d.mass), friction(d.friction), restitution(d.restitution),
              linearDamping(d.linearDamping), angularDamping(d.angularDamping) {}

        Vector3 position() const override { return desc.position; }
        Quaternion rotation() const override { return desc.rotation; }
        void setTransform(const Vector3& p, const Quaternion& r) override
        {
            desc.position = p;
            desc.rotation = r;
        }
        Vector3 linearVelocity() const override { return linear; }
        void setLinearVelocity(const Vector3& value) override { linear = value; }
        Vector3 angularVelocity() const override { return angular; }
        void setAngularVelocity(const Vector3& value) override { angular = value; }
        void applyForce(const Vector3& value) override { force += value; ++forceCalls; }
        // A unit inertia, so a torque impulse adds to the angular velocity as it is.
        void applyImpulse(const Vector3& value) override { linear += value / mass; }
        void applyTorque(const Vector3& value) override { torque += value; ++torqueCalls; }
        void applyTorqueImpulse(const Vector3& value) override { angular += value; }
        void setFriction(const float value) override { friction = value; ++settingCalls; }
        void setRestitution(const float value) override { restitution = value; ++settingCalls; }
        void setDamping(const float linearValue, const float angularValue) override
        {
            linearDamping = linearValue;
            angularDamping = angularValue;
            ++settingCalls;
        }
        bool setMass(const float value) override
        {
            ++settingCalls;
            if (value <= 0.0f) {
                return false;   // as a backend that derives a zero mass from the shape
            }
            mass = value;
            return true;
        }
        void activate() override {}
        bool isActive() const override { return true; }

        PhysicsBodyDesc desc;
        float mass;
        float friction;
        float restitution;
        float linearDamping;
        float angularDamping;
        Vector3 linear = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 angular = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 force = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 torque = Vector3(0.0f, 0.0f, 0.0f);
        int forceCalls = 0;
        int torqueCalls = 0;
        int settingCalls = 0;
    };

    class RecordingWorld final : public PhysicsWorld
    {
    public:
        void step(float /*dt*/) override {}
        void setGravity(const Vector3& /*gravity*/) override {}
        Vector3 gravity() const override { return Vector3(0.0f, -9.81f, 0.0f); }

        PhysicsBody* createBody(const PhysicsBodyDesc& desc) override
        {
            ++created;
            bodies.push_back(std::make_unique<RecordingBody>(desc));
            return bodies.back().get();
        }
        void destroyBody(PhysicsBody* body) override
        {
            ++destroyed;
            std::erase_if(bodies, [body](const auto& owned) { return owned.get() == body; });
        }
        PhysicsJoint* createJoint(const PhysicsJointDesc& /*desc*/) override { return nullptr; }
        void destroyJoint(PhysicsJoint* /*joint*/) override {}
        std::optional<PhysicsRaycastHit> raycastFirst(const Vector3& /*start*/, const Vector3& /*end*/) const override
        {
            return std::nullopt;
        }
        std::vector<PhysicsRaycastHit> raycastAll(const Vector3& /*start*/, const Vector3& /*end*/) const override
        {
            return {};
        }

        std::vector<std::unique_ptr<RecordingBody>> bodies;
        int created = 0;
        int destroyed = 0;
    };

    RigidBodyComponent* addDynamicBody(Entity& root)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        root.addChild(std::move(owned));
        auto* body = static_cast<RigidBodyComponent*>(entity->addComponentInstance(
            std::make_unique<RigidBodyComponent>(nullptr, entity), componentTypeID<RigidBodyComponent>()));
        body->setType(RigidBodyType::Dynamic);
        return body;
    }

    RecordingBody* bodyOf(const RigidBodyComponent* component)
    {
        return static_cast<RecordingBody*>(component->physicsBody());
    }

    bool same(const Vector3& a, const Vector3& b) { return near(a, b, 1e-5f); }
}

int main()
{
    std::cout << "calls made before the body exists\n";
    {
        // The world outlives the entities: a component destroys its body on the way out.
        RecordingWorld world;
        Entity root;
        root.setEnabledInHierarchy(true);
        RigidBodyComponent* projectile = addDynamicBody(root);
        projectile->setMass(2.0f);
        projectile->setLinearVelocity(Vector3(0.0f, 0.0f, -20.0f));
        projectile->setAngularVelocity(Vector3(1.0f, 0.0f, 0.0f));
        projectile->applyImpulse(Vector3(4.0f, 0.0f, 0.0f));
        projectile->applyImpulse(Vector3(2.0f, 0.0f, 0.0f));
        projectile->applyTorqueImpulse(Vector3(0.0f, 0.5f, 0.0f));
        projectile->applyForce(Vector3(0.0f, 10.0f, 0.0f));
        projectile->applyTorque(Vector3(0.0f, 0.0f, 3.0f));
        check(projectile->physicsBody() == nullptr, "no body exists before the first sync");
        check(same(projectile->linearVelocity(), Vector3(0.0f, 0.0f, -20.0f)) &&
                  same(projectile->angularVelocity(), Vector3(1.0f, 0.0f, 0.0f)),
            "the getters answer the velocity the body will be given");

        projectile->syncFromSimulation(world);
        RecordingBody* body = bodyOf(projectile);
        if (check(body != nullptr && world.created == 1, "the first sync creates the body")) {
            check(same(body->linear, Vector3(3.0f, 0.0f, -20.0f)),
                "it is created with the held velocity plus the held impulses over its mass");
            check(same(body->angular, Vector3(1.0f, 0.5f, 0.0f)), "and the held angular velocity and torque impulse");
            check(body->forceCalls == 1 && same(body->force, Vector3(0.0f, 10.0f, 0.0f)) &&
                      body->torqueCalls == 1 && same(body->torque, Vector3(0.0f, 0.0f, 3.0f)),
                "a held force and torque reach the body once, before its first step");

            projectile->syncFromSimulation(world);
            check(body->forceCalls == 1 && same(body->linear, Vector3(3.0f, 0.0f, -20.0f)),
                "nothing held is applied a second time");

            projectile->applyImpulse(Vector3(0.0f, 2.0f, 0.0f));
            check(same(body->linear, Vector3(3.0f, 1.0f, -20.0f)), "once the body exists, calls reach it directly");
        }

        RigidBodyComponent* overridden = addDynamicBody(root);
        overridden->applyImpulse(Vector3(5.0f, 0.0f, 0.0f));
        overridden->setLinearVelocity(Vector3(0.0f, 1.0f, 0.0f));
        overridden->syncFromSimulation(world);
        check(bodyOf(overridden) && same(bodyOf(overridden)->linear, Vector3(0.0f, 1.0f, 0.0f)),
            "a velocity set after an impulse replaces it, as it would on a live body");

        RigidBodyComponent* teleported = addDynamicBody(root);
        teleported->setLinearVelocity(Vector3(9.0f, 0.0f, 0.0f));
        teleported->applyImpulse(Vector3(1.0f, 0.0f, 0.0f));
        teleported->teleport(Vector3(1.0f, 2.0f, 3.0f));
        teleported->syncFromSimulation(world);
        check(bodyOf(teleported) && same(bodyOf(teleported)->linear, Vector3(0.0f, 0.0f, 0.0f)) &&
                  same(bodyOf(teleported)->desc.position, Vector3(1.0f, 2.0f, 3.0f)),
            "teleport before the body exists stops it at the new position");
    }

    std::cout << "\nsettings on a live body\n";
    {
        // The world outlives the entities: a component destroys its body on the way out.
        RecordingWorld world;
        Entity root;
        root.setEnabledInHierarchy(true);
        RigidBodyComponent* component = addDynamicBody(root);
        component->syncFromSimulation(world);
        RecordingBody* body = bodyOf(component);
        if (check(body != nullptr, "the body exists")) {
            body->linear = Vector3(7.0f, 0.0f, 0.0f);
            body->angular = Vector3(0.0f, 2.0f, 0.0f);

            component->setFriction(0.8f);
            component->setRestitution(0.25f);
            component->setLinearDamping(0.1f);
            component->setAngularDamping(0.3f);
            component->setMass(3.0f);
            component->syncFromSimulation(world);
            check(bodyOf(component) == body && world.created == 1 && world.destroyed == 0,
                "mass, friction, restitution and damping rebuild nothing");
            check(body->friction == 0.8f && body->restitution == 0.25f && body->linearDamping == 0.1f &&
                      body->angularDamping == 0.3f && body->mass == 3.0f,
                "each reaches the live body");
            check(same(body->linear, Vector3(7.0f, 0.0f, 0.0f)), "and the body keeps moving as it was");

            const int calls = body->settingCalls;
            component->setFriction(0.8f);
            component->setRestitution(0.25f);
            component->setLinearDamping(0.1f);
            component->setAngularDamping(0.3f);
            component->setMass(3.0f);
            check(body->settingCalls == calls, "an unchanged value reaches nothing");

            component->setMass(0.0f);   // refused in place: the body is rebuilt
            check(same(component->linearVelocity(), Vector3(7.0f, 0.0f, 0.0f)),
                "a body awaiting its rebuild reports the velocity it still has");
            component->syncFromSimulation(world);
            RecordingBody* rebuilt = bodyOf(component);
            check(world.created == 2 && world.destroyed == 1 && rebuilt != nullptr && rebuilt->desc.mass == 0.0f,
                "a mass the body cannot take in place rebuilds it with that mass");
            check(rebuilt && same(rebuilt->linear, Vector3(7.0f, 0.0f, 0.0f)) &&
                      same(rebuilt->angular, Vector3(0.0f, 2.0f, 0.0f)),
                "the rebuilt body carries the old one's velocities");
            check(rebuilt && rebuilt->friction == 0.8f && rebuilt->linearDamping == 0.1f,
                "and every other setting");
        }

        RigidBodyComponent* fixed = addDynamicBody(root);
        fixed->setType(RigidBodyType::Static);
        fixed->syncFromSimulation(world);
        RecordingBody* staticBody = bodyOf(fixed);
        const int createdBefore = world.created;
        fixed->setMass(12.0f);
        fixed->syncFromSimulation(world);
        check(staticBody && staticBody->settingCalls == 0 && world.created == createdBefore,
            "a static body is neither told its mass nor rebuilt for it");
        check(fixed->mass() == 12.0f, "the mass is kept for a later type change");
    }

    return finish("rigid-body state");
}
