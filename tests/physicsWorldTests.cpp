// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The physics seam, exercised through whatever backend the build supplies.
//
// These are the properties an application depends on and that a stub silently
// fails: a dynamic body falls, it STOPS on a static one rather than passing
// through, restitution makes it bounce, a kinematic body does not move itself,
// and a raycast reports the nearest body with a sensible normal.
//
#include <cmath>
#include <cstdio>
#include <memory>

#include "framework/physics/jolt/joltPhysicsWorld.h"
#include "framework/physics/physicsWorld.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        if (!condition) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    }

    /// Advance one simulated second at 60 Hz.
    void stepSeconds(PhysicsWorld& world, const float seconds)
    {
        const int steps = static_cast<int>(seconds * 60.0f);
        for (int i = 0; i < steps; ++i) {
            world.step(1.0f / 60.0f);
        }
    }

    PhysicsBody* addGround(PhysicsWorld& world)
    {
        PhysicsBodyDesc desc;
        desc.shape = PhysicsShapeType::Box;
        desc.motion = PhysicsMotionType::Static;
        desc.halfExtents = Vector3(50.0f, 0.5f, 50.0f);
        desc.position = Vector3(0.0f, -0.5f, 0.0f);
        return world.createBody(desc);
    }

    PhysicsBody* addSphere(PhysicsWorld& world, const float height, const float restitution)
    {
        PhysicsBodyDesc desc;
        desc.shape = PhysicsShapeType::Sphere;
        desc.motion = PhysicsMotionType::Dynamic;
        desc.radius = 0.5f;
        desc.position = Vector3(0.0f, height, 0.0f);
        desc.restitution = restitution;
        return world.createBody(desc);
    }
}

int main()
{
    const auto world = createJoltPhysicsWorld();
    if (!world) {
        std::printf("physics: no backend in this build, nothing to test\n");
        return 0;
    }

    check(std::abs(world->gravity().getY() + 9.81f) < 0.01f, "default gravity is -9.81 on Y");

    // A dynamic body falls, and free fall matches 0.5*g*t^2 within a few percent.
    {
        PhysicsBody* ball = addSphere(*world, 10.0f, 0.0f);
        check(ball != nullptr, "the world creates a dynamic sphere");
        if (ball != nullptr) {
            stepSeconds(*world, 0.5f);
            const float y = ball->position().getY();
            const float expected = 10.0f - 0.5f * 9.81f * 0.25f;
            check(std::abs(y - expected) < 0.2f, "half a second of free fall matches 0.5*g*t^2");
            world->destroyBody(ball);
        }
    }

    // A dynamic body lands ON a static one instead of through it, and settles.
    {
        PhysicsBody* ground = addGround(*world);
        PhysicsBody* ball = addSphere(*world, 5.0f, 0.0f);
        check(ground != nullptr && ball != nullptr, "ground and ball are created");
        if (ground != nullptr && ball != nullptr) {
            stepSeconds(*world, 4.0f);
            const float y = ball->position().getY();
            // Resting on a ground whose top face is y = 0, so the centre sits at
            // the sphere's radius.
            check(std::abs(y - 0.5f) < 0.05f, "the ball rests on the ground at its radius");
            check(std::abs(ball->linearVelocity().getY()) < 0.1f, "the resting ball has stopped");

            // Raycast straight down from above: the nearest hit is the ball, and
            // its normal points back up at the ray.
            const auto hit = world->raycastFirst(Vector3(0.0f, 5.0f, 0.0f), Vector3(0.0f, -5.0f, 0.0f));
            check(hit.has_value(), "a downward ray hits something");
            if (hit) {
                check(std::abs(hit->point.getY() - 1.0f) < 0.1f, "the ray hits the top of the ball");
                check(hit->normal.getY() > 0.9f, "the hit normal points up");
            }

            const auto all = world->raycastAll(Vector3(0.0f, 5.0f, 0.0f), Vector3(0.0f, -5.0f, 0.0f));
            check(all.size() >= 2, "the same ray passes through the ball and into the ground");
            if (all.size() >= 2) {
                check(all[0].fraction <= all[1].fraction, "raycastAll reports nearest first");
            }

            // An impulse straight up gets it moving again.
            ball->applyImpulse(Vector3(0.0f, 20.0f, 0.0f));
            ball->activate();
            world->step(1.0f / 60.0f);
            check(ball->linearVelocity().getY() > 1.0f, "an upward impulse lifts the resting ball");

            world->destroyBody(ball);
            world->destroyBody(ground);
        }
    }

    // Restitution bounces. A bouncy ball dropped from the same height ends up
    // higher after the first contact than a dead one.
    {
        PhysicsBody* ground = addGround(*world);
        PhysicsBody* bouncy = addSphere(*world, 5.0f, 0.9f);
        if (ground != nullptr && bouncy != nullptr) {
            stepSeconds(*world, 1.2f);
            check(bouncy->position().getY() > 0.8f, "a bouncy ball rebounds off the ground");
            world->destroyBody(bouncy);
            world->destroyBody(ground);
        }
    }

    // A kinematic body ignores gravity and stays where it was put.
    {
        PhysicsBodyDesc desc;
        desc.shape = PhysicsShapeType::Box;
        desc.motion = PhysicsMotionType::Kinematic;
        desc.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
        desc.position = Vector3(3.0f, 4.0f, 0.0f);
        PhysicsBody* platform = world->createBody(desc);
        check(platform != nullptr, "the world creates a kinematic box");
        if (platform != nullptr) {
            stepSeconds(*world, 1.0f);
            check(std::abs(platform->position().getY() - 4.0f) < 1e-3f,
                "a kinematic body does not fall");
            world->destroyBody(platform);
        }
    }

    // A ray into empty space reports nothing.
    {
        const auto miss = world->raycastFirst(Vector3(100.0f, 100.0f, 100.0f),
                                              Vector3(100.0f, 90.0f, 100.0f));
        check(!miss.has_value(), "a ray through empty space reports no hit");
    }

    // ── Joints ──────────────────────────────────────────────────────────────
    // A fixed joint welds two bodies: the upper one must not fall away from the
    // lower, which is the whole point of the constraint.
    {
        PhysicsBody* ground = addGround(*world);
        PhysicsBodyDesc desc;
        desc.shape = PhysicsShapeType::Box;
        desc.motion = PhysicsMotionType::Dynamic;
        desc.halfExtents = Vector3(0.35f, 0.35f, 0.35f);
        desc.mass = 8.0f;
        desc.position = Vector3(0.0f, 3.0f, 0.0f);
        PhysicsBody* lower = world->createBody(desc);
        desc.position = Vector3(0.0f, 3.7f, 0.0f);
        PhysicsBody* upper = world->createBody(desc);

        PhysicsJointDesc joint;
        joint.type = PhysicsJointType::Fixed;
        joint.bodyA = upper;
        joint.bodyB = lower;
        joint.framePosition = Vector3(0.0f, 3.35f, 0.0f);
        PhysicsJoint* weld = world->createJoint(joint);
        check(weld != nullptr, "the world creates a fixed joint");

        if (lower != nullptr && upper != nullptr && weld != nullptr) {
            stepSeconds(*world, 2.0f);
            const float gap = upper->position().getY() - lower->position().getY();
            check(std::abs(gap - 0.7f) < 0.05f, "a fixed joint holds its two bodies apart at 0.7");
            check(!weld->isBroken(), "an unbreakable weld does not break under its own weight");
            world->destroyJoint(weld);
        }
        world->destroyBody(upper);
        world->destroyBody(lower);
        world->destroyBody(ground);
    }

    // A hinge pinned to the world holds its body at the joint and lets it swing.
    {
        PhysicsBodyDesc desc;
        desc.shape = PhysicsShapeType::Box;
        desc.motion = PhysicsMotionType::Dynamic;
        desc.halfExtents = Vector3(0.1f, 0.1f, 0.8f);
        desc.mass = 4.0f;
        // Offset along +Z: the primary axis is world +X, so a centre on that axis
        // would sit ON the pivot and could not swing at all.
        desc.position = Vector3(0.0f, 5.0f, 0.8f);
        PhysicsBody* arm = world->createBody(desc);

        PhysicsJointDesc joint;
        joint.type = PhysicsJointType::Hinge;
        joint.bodyA = arm;
        joint.bodyB = nullptr;   // pinned to the world
        joint.framePosition = Vector3(0.0f, 5.0f, 0.0f);
        // Default frame rotation: the primary axis is world +X, so the arm swings
        // in the YZ plane and cannot translate away from the pivot.
        PhysicsJoint* hinge = world->createJoint(joint);
        check(hinge != nullptr, "the world creates a hinge pinned to the world");

        if (arm != nullptr && hinge != nullptr) {
            stepSeconds(*world, 1.5f);
            const Vector3 p = arm->position();
            const float radius = std::sqrt(p.getX() * p.getX() +
                (p.getY() - 5.0f) * (p.getY() - 5.0f) + p.getZ() * p.getZ());
            check(std::abs(radius - 0.8f) < 0.1f, "the hinged arm stays 0.8 from its pivot");
            check(p.getY() < 4.9f, "the hinged arm swung down under gravity");
            world->destroyJoint(hinge);
        }
        world->destroyBody(arm);
    }

    // A slider motor drives its body along the joint's primary axis and nowhere
    // else.
    {
        PhysicsBodyDesc desc;
        desc.shape = PhysicsShapeType::Box;
        desc.motion = PhysicsMotionType::Dynamic;
        desc.halfExtents = Vector3(0.4f, 0.15f, 0.3f);
        desc.mass = 10.0f;
        desc.position = Vector3(0.0f, 8.0f, 0.0f);
        PhysicsBody* platform = world->createBody(desc);

        PhysicsJointDesc joint;
        joint.type = PhysicsJointType::Slider;
        joint.bodyA = platform;
        joint.bodyB = nullptr;
        joint.framePosition = Vector3(0.0f, 8.0f, 0.0f);
        joint.enableLimits = true;
        joint.minLimit = -2.0f;
        joint.maxLimit = 2.0f;
        joint.motorSpeed = 1.5f;
        joint.maxMotorForce = 400.0f;
        PhysicsJoint* slider = world->createJoint(joint);
        check(slider != nullptr, "the world creates a motorised slider");

        if (platform != nullptr && slider != nullptr) {
            stepSeconds(*world, 1.0f);
            const Vector3 p = platform->position();
            check(p.getX() > 0.5f, "the slider motor drove the platform along +X");
            check(std::abs(p.getY() - 8.0f) < 0.1f, "the slider carries the platform's weight");
            check(p.getX() <= 2.1f, "the slider stays inside its limit");

            // Reversing the motor sends it back the other way.
            const float reached = p.getX();
            slider->setMotorSpeed(-1.5f);
            stepSeconds(*world, 1.0f);
            check(platform->position().getX() < reached, "reversing the motor reverses the travel");
            world->destroyJoint(slider);
        }
        world->destroyBody(platform);
    }

    // A weld with a low threshold breaks when it has to carry a heavy body.
    {
        PhysicsBodyDesc anchorDesc;
        anchorDesc.shape = PhysicsShapeType::Box;
        anchorDesc.motion = PhysicsMotionType::Static;
        anchorDesc.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
        anchorDesc.position = Vector3(20.0f, 10.0f, 0.0f);
        PhysicsBody* anchor = world->createBody(anchorDesc);

        PhysicsBodyDesc heavyDesc;
        heavyDesc.shape = PhysicsShapeType::Box;
        heavyDesc.motion = PhysicsMotionType::Dynamic;
        heavyDesc.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
        heavyDesc.mass = 500.0f;
        heavyDesc.position = Vector3(20.0f, 8.5f, 0.0f);
        PhysicsBody* heavy = world->createBody(heavyDesc);

        PhysicsJointDesc joint;
        joint.type = PhysicsJointType::Fixed;
        joint.bodyA = heavy;
        joint.bodyB = anchor;
        joint.framePosition = Vector3(20.0f, 9.25f, 0.0f);
        joint.breakImpulse = 1.0f;   // far below what 500 kg needs
        PhysicsJoint* weld = world->createJoint(joint);

        if (weld != nullptr && heavy != nullptr) {
            stepSeconds(*world, 1.0f);
            check(weld->isBroken(), "a weld under more than its break impulse breaks");
            check(heavy->position().getY() < 8.0f, "the body falls once the weld breaks");
            world->destroyJoint(weld);
        }
        world->destroyBody(heavy);
        world->destroyBody(anchor);
    }

    // Destroying a body takes its joints with it: a constraint left pointing at a
    // freed body is a dangling reference inside the solver.
    {
        PhysicsBodyDesc desc;
        desc.shape = PhysicsShapeType::Sphere;
        desc.motion = PhysicsMotionType::Dynamic;
        desc.radius = 0.3f;
        desc.position = Vector3(30.0f, 5.0f, 0.0f);
        PhysicsBody* a = world->createBody(desc);
        desc.position = Vector3(30.0f, 4.4f, 0.0f);
        PhysicsBody* b = world->createBody(desc);

        PhysicsJointDesc joint;
        joint.type = PhysicsJointType::Ball;
        joint.bodyA = a;
        joint.bodyB = b;
        joint.framePosition = Vector3(30.0f, 4.7f, 0.0f);
        check(world->createJoint(joint) != nullptr, "the world creates a ball joint");

        world->destroyBody(b);          // the joint must go with it
        stepSeconds(*world, 0.5f);      // would walk a dangling constraint
        check(a->position().getY() < 5.0f, "the surviving body falls freely afterwards");
        world->destroyBody(a);
    }

    // Worlds are created and destroyed repeatedly by anything that reloads a
    // scene, and the backend spins up a job system per world; a teardown that
    // races its own workers shows up here rather than as a crash on exit.
    for (int i = 0; i < 8; ++i) {
        const auto scratch = createJoltPhysicsWorld();
        check(scratch != nullptr, "a second world can be created");
        if (scratch) {
            PhysicsBody* ground = addGround(*scratch);
            PhysicsBody* ball = addSphere(*scratch, 3.0f, 0.0f);
            stepSeconds(*scratch, 0.5f);
            check(ball != nullptr && ball->position().getY() < 3.0f,
                "the ball in the scratch world fell");
            (void)ground;
            // Deliberately NOT destroying the bodies: the world owns them and has
            // to clean up after itself.
        }
    }

    if (failures == 0) {
        std::printf("physics world tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
