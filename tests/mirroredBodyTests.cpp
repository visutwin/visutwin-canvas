// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A dynamic body on an entity with a negative scale, or under a mirrored ancestor,
// must keep the entity's orientation across physics steps (upstream #9500). The
// rotation read from a mirrored world transform is NOT the entity's rotation:
// Quaternion::fromMatrix4 negates the X axis of a mirrored basis to make it a rotation,
// and a pair of negative scale factors reads as a 180-degree turn. The body is created
// with that rotation, so writing it back as the WORLD rotation baked the correction into
// the local rotation and the entity turned on its first step.
//
// A stub world holds each body at the pose it was created with and, per step, turns it
// by a set world-space rotation. With no turn, one step must leave every entity's world
// transform where it was; with a turn, the entity's basis must turn by exactly that.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/appOptions.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/physics/physicsWorld.h"
#include "platform/graphics/graphicsDevice.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>&, int,
            const VertexBufferOptions&) override { return nullptr; }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int, const std::vector<uint8_t>&) override
        {
            return nullptr;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

    class HoldBody final : public PhysicsBody
    {
    public:
        Vector3 pos;
        Quaternion rot;
        Vector3 position() const override { return pos; }
        Quaternion rotation() const override { return rot; }
        void setTransform(const Vector3& p, const Quaternion& r) override { pos = p; rot = r; }
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

    // Holds every body where it was created; step() turns each by `turn` (world space).
    class HoldWorld final : public PhysicsWorld
    {
    public:
        Quaternion turn = Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
        std::vector<std::unique_ptr<HoldBody>> bodies;

        void step(float) override
        {
            for (auto& body : bodies) {
                body->rot = (turn * body->rot).normalized();
            }
        }
        void setGravity(const Vector3&) override {}
        Vector3 gravity() const override { return Vector3(0.0f, 0.0f, 0.0f); }
        PhysicsBody* createBody(const PhysicsBodyDesc& desc) override
        {
            auto body = std::make_unique<HoldBody>();
            body->pos = desc.position;
            body->rot = desc.rotation;
            bodies.push_back(std::move(body));
            return bodies.back().get();
        }
        void destroyBody(PhysicsBody* body) override
        {
            std::erase_if(bodies, [body](const auto& b) { return b.get() == body; });
        }
        PhysicsJoint* createJoint(const PhysicsJointDesc&) override { return nullptr; }
        void destroyJoint(PhysicsJoint*) override {}
        std::optional<PhysicsRaycastHit> raycastFirst(const Vector3&, const Vector3&) const override
        {
            return std::nullopt;
        }
        std::vector<PhysicsRaycastHit> raycastAll(const Vector3&, const Vector3&) const override { return {}; }
    };

    Entity* addEntity(Engine& engine, GraphNode* parent)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        parent->addChild(std::move(owned));
        return entity;
    }

    float maxDifference(const Matrix4& a, const Matrix4& b)
    {
        float worst = 0.0f;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                worst = std::max(worst, std::fabs(a.getElement(c, r) - b.getElement(c, r)));
            }
        }
        return worst;
    }

    struct Case
    {
        const char* name;
        Vector3 parentScale;
        Vector3 localScale;
    };
}

int main()
{
    auto engine = std::make_shared<Engine>(nullptr);
    auto world = std::make_shared<HoldWorld>();
    AppOptions options;
    options.graphicsDevice = std::make_shared<StubDevice>();
    options.physicsWorld = world;
    options.registerComponentSystem<CollisionComponentSystem>();
    options.registerComponentSystem<RigidBodyComponentSystem>();
    engine->init(options);
    auto* system = dynamic_cast<RigidBodyComponentSystem*>(engine->systems()->getById("rigidbody"));
    if (!system) {
        std::cout << "  FAIL the rigid-body system exists\n";
        return 1;
    }

    const Case cases[] = {
        {"unmirrored (control)", Vector3(1.0f, 1.0f, 1.0f), Vector3(1.0f, 1.0f, 1.0f)},
        {"negative local X", Vector3(1.0f, 1.0f, 1.0f), Vector3(-1.0f, 1.0f, 1.0f)},
        {"negative local Y", Vector3(1.0f, 1.0f, 1.0f), Vector3(1.0f, -1.0f, 1.0f)},
        {"negative local Y and Z (reads as a 180-degree turn)", Vector3(1.0f, 1.0f, 1.0f), Vector3(1.0f, -2.0f, -1.0f)},
        {"mirrored parent", Vector3(-1.0f, 1.0f, 1.0f), Vector3(1.0f, 1.0f, 1.0f)},
        {"mirrored parent and mirrored child", Vector3(1.0f, 1.0f, -1.0f), Vector3(1.0f, -1.0f, 1.0f)},
    };

    const Quaternion turn = Quaternion::fromEulerAngles(0.0f, 30.0f, 0.0f);

    for (const auto& c : cases) {
        std::cout << c.name << '\n';
        Entity* parent = addEntity(*engine, engine->root());
        parent->setLocalPosition(1.0f, 2.0f, 3.0f);
        parent->setLocalEulerAngles(10.0f, 20.0f, 30.0f);
        parent->setLocalScale(c.parentScale);
        Entity* child = addEntity(*engine, parent);
        child->setLocalPosition(0.5f, -1.0f, 2.0f);
        child->setLocalEulerAngles(-15.0f, 40.0f, 5.0f);
        child->setLocalScale(c.localScale);
        auto* collision = static_cast<CollisionComponent*>(child->addComponent<CollisionComponent>());
        collision->setType("box");
        auto* body = static_cast<RigidBodyComponent*>(child->addComponent<RigidBodyComponent>());
        body->setType(RigidBodyType::Dynamic);   // static by default, as upstream

        const Matrix4 before = child->worldTransform();
        world->turn = Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
        system->step(1.0f / 60.0f);
        const Matrix4 afterStill = child->worldTransform();
        check(maxDifference(before, afterStill) < 1e-4f,
            std::string("  a step that does not move the body leaves the entity where it was (max diff ")
            + std::to_string(maxDifference(before, afterStill)) + ")");

        world->turn = turn;
        system->step(1.0f / 60.0f);
        const Matrix4 afterTurn = child->worldTransform();
        // The body turned by `turn` in place, so every basis column turns by it and the
        // translation stays.
        Matrix4 expected = afterStill;
        for (int col = 0; col < 3; ++col) {
            const Vector3 axis(afterStill.getColumn(col));
            const Vector3 turned = turn * axis;
            expected.setElement(col, 0, turned.getX());
            expected.setElement(col, 1, turned.getY());
            expected.setElement(col, 2, turned.getZ());
        }
        check(maxDifference(expected, afterTurn) < 1e-4f,
            std::string("  a turned body turns the entity's basis by exactly that (max diff ")
            + std::to_string(maxDifference(expected, afterTurn)) + ")");

        auto removed = engine->root()->removeChild(parent);   // destroyed here, bodies and all
    }

    std::cout << (failures == 0 ? "\nAll mirrored body tests passed\n" : "\nMirrored body tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
