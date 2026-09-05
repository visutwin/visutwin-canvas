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
