// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A PhysicsWorld frees every joint touching a body it destroys. Until 2026-09-23
// nothing told the JointComponent, which kept the freed pointer and called
// isBroken() on it at its next update — after any rigid-body setter that rebuilds
// the body (setMass, say) or the destruction of either end's entity. The joint was
// never rebuilt against the new body either.
//
// A use-after-free cannot be seen in a result, so this runs the components against
// a world that NEVER frees its joint objects: it marks one dead exactly when a real
// world would free it and counts every call made on a dead one. The component-side
// contract under test: a rebuilt body gets its joint rebuilt, and a destroyed end
// leaves no joint and no reference to it.

#include <iostream>
#include <memory>
#include <optional>
#include <vector>

#include "framework/components/component.h"
#include "framework/components/joint/jointComponent.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/entity.h"
#include "framework/physics/physicsWorld.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    class FakeBody final : public PhysicsBody
    {
    public:
        Vector3 position() const override { return Vector3(0.0f, 0.0f, 0.0f); }
        Quaternion rotation() const override { return Quaternion(0.0f, 0.0f, 0.0f, 1.0f); }
        void setTransform(const Vector3&, const Quaternion&) override {}
        Vector3 linearVelocity() const override { return Vector3(0.0f, 0.0f, 0.0f); }
        void setLinearVelocity(const Vector3&) override {}
        Vector3 angularVelocity() const override { return Vector3(0.0f, 0.0f, 0.0f); }
        void setAngularVelocity(const Vector3&) override {}
        void applyForce(const Vector3&) override {}
        void applyImpulse(const Vector3&) override {}
        void applyTorque(const Vector3&) override {}
        void activate() override {}
        bool isActive() const override { return true; }
    };

    int deadCalls = 0;

    class FakeJoint final : public PhysicsJoint
    {
    public:
        FakeJoint(const PhysicsBody* a, const PhysicsBody* b) : bodyA(a), bodyB(b) {}
        void setEnabled(bool) override { touch(); }
        bool enabled() const override { touch(); return true; }
        void setMotorSpeed(float) override { touch(); }
        float motorSpeed() const override { touch(); return 0.0f; }
        bool isBroken() const override { touch(); return false; }

        const PhysicsBody* bodyA;
        const PhysicsBody* bodyB;
        bool alive = true;

    private:
        void touch() const { deadCalls += alive ? 0 : 1; }
    };

    /// Keeps every body and joint allocated for the life of the test, so a stale
    /// pointer never reads freed memory — it reads an object marked dead.
    class TrackingWorld final : public PhysicsWorld
    {
    public:
        void step(float) override {}
        void setGravity(const Vector3&) override {}
        Vector3 gravity() const override { return Vector3(0.0f, -9.81f, 0.0f); }

        PhysicsBody* createBody(const PhysicsBodyDesc&) override
        {
            bodies.push_back(std::make_unique<FakeBody>());
            return bodies.back().get();
        }

        // As the real world: a destroyed body takes every joint touching it along.
        void destroyBody(PhysicsBody* body) override
        {
            for (const auto& joint : joints) {
                if (joint->alive && (joint->bodyA == body || joint->bodyB == body)) {
                    joint->alive = false;
                }
            }
        }

        PhysicsJoint* createJoint(const PhysicsJointDesc& desc) override
        {
            joints.push_back(std::make_unique<FakeJoint>(desc.bodyA, desc.bodyB));
            return joints.back().get();
        }

        void destroyJoint(PhysicsJoint* joint) override
        {
            auto* fake = static_cast<FakeJoint*>(joint);
            deadCalls += fake->alive ? 0 : 1;   // destroying an already-freed joint
            fake->alive = false;
        }

        std::optional<PhysicsRaycastHit> raycastFirst(const Vector3&, const Vector3&) const override
        {
            return std::nullopt;
        }
        std::vector<PhysicsRaycastHit> raycastAll(const Vector3&, const Vector3&) const override { return {}; }

        int liveJoints() const
        {
            int count = 0;
            for (const auto& joint : joints) {
                count += joint->alive ? 1 : 0;
            }
            return count;
        }

        std::vector<std::unique_ptr<FakeBody>> bodies;
        std::vector<std::unique_ptr<FakeJoint>> joints;
    };

    Entity* addEntity(Entity& root)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        root.addChild(std::move(owned));
        return entity;
    }

    RigidBodyComponent* addRigidBody(Entity* entity)
    {
        return static_cast<RigidBodyComponent*>(entity->addComponentInstance(
            std::make_unique<RigidBodyComponent>(nullptr, entity), componentTypeID<RigidBodyComponent>()));
    }
}

int main()
{
    TrackingWorld world;
    Entity root;
    root.setEnabledInHierarchy(true);

    Entity* endA = addEntity(root);
    Entity* endB = addEntity(root);
    Entity* frame = addEntity(root);
    auto* rigidA = addRigidBody(endA);
    auto* rigidB = addRigidBody(endB);
    auto* joint = static_cast<JointComponent*>(frame->addComponentInstance(
        std::make_unique<JointComponent>(nullptr, frame), componentTypeID<JointComponent>()));
    joint->setType(PhysicsJointType::Hinge);
    joint->setEntityA(endA);
    joint->setEntityB(endB);

    // One frame: the bodies step first, then the joints (the system order).
    const auto frameUpdate = [&]() {
        if (endA->findComponent<RigidBodyComponent>()) { rigidA->syncFromSimulation(world); }
        if (endB->findComponent<RigidBodyComponent>()) { rigidB->syncFromSimulation(world); }
        joint->syncToSimulation(world);
    };

    std::cout << "joint over two rigid bodies\n";
    frameUpdate();
    check(world.liveJoints() == 1 && world.joints.size() == 1, "the joint is created once both bodies exist");

    std::cout << "\nrebuilding one end's body\n";
    rigidA->setMass(5.0f);          // marks the body stale: it is destroyed and recreated
    frameUpdate();
    frameUpdate();
    check(deadCalls == 0, "no call reaches the joint the world freed with the old body");
    check(world.liveJoints() == 1, "exactly one joint is alive");
    check(world.joints.size() == 2 && world.joints.back()->alive &&
              world.joints.back()->bodyA == rigidA->physicsBody(),
        "it was rebuilt against the NEW body");

    std::cout << "\ndestroying one end's entity\n";
    endB->destroy();                // frees B's body, and the world frees the joint with it
    frameUpdate();
    frameUpdate();
    check(deadCalls == 0, "no call reaches the freed joint after an end is destroyed");
    check(world.liveJoints() == 0, "no joint remains");
    check(world.joints.size() == 2, "and none is rebuilt with the end silently pinned to the world");
    check(joint->entityB() == nullptr, "the joint no longer names the destroyed entity");

    std::cout << "\nsetting a new end\n";
    Entity* endC = addEntity(root);
    auto* rigidC = addRigidBody(endC);
    joint->setEntityB(endC);
    rigidC->syncFromSimulation(world);
    frameUpdate();
    check(world.liveJoints() == 1 && world.joints.back()->bodyB == rigidC->physicsBody(),
        "a new end brings the joint back");
    check(deadCalls == 0, "still no call on a freed joint");

    std::cout << (failures == 0 ? "\nAll joint lifetime tests passed\n" : "\nJoint lifetime tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
