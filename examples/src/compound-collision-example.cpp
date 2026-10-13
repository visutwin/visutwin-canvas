// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream physics/compound-collision.
//
// A chair is one dynamic rigid body (mass 10, friction 0.5, restitution 0.5) whose collision
// is a COMPOUND: no shape of its own, made of the collision shapes of its children, each where
// its entity sits: a seat box, a back box, two short front legs and two long back legs
// (cylinders). The render boxes and cylinders are grandchildren, scaled to match, so the
// collision entities keep unit scale. A chair falls at once, and every quarter second a clone
// of it is dropped from 1-3 m, up to twenty. Every frame the render components under an active
// body are drawn red, those under a sleeping one grey. A 10 x 10 static ground, a shadowed
// directional light (distance 8), grey ambient, the camera 7 m back and 4 up looking 30
// degrees down.
//
// DEVIATIONS:
// - The clones' positions come from a fixed-seed generator rather than Math.random(), so
//   every run drops them the same way.
// - Upstream's physics engine reports its STATIC ground as active, so the ground is drawn red;
//   Jolt's static bodies are never active, so a static body counts as active here.
//
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/componentRegistry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/physics/jolt/joltPhysicsWorld.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class CompoundCollisionExample final : public ExampleApp
{
public:
    CompoundCollisionExample() : ExampleApp({.title = "Compound Collision"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<CollisionComponentSystem>();
        options.registerComponentSystem<RigidBodyComponentSystem>();
        options.physicsWorld = createJoltPhysicsWorld();
    }

    bool create() override
    {
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);
        _red = material(Color(0.7f, 0.3f, 0.3f, 1.0f));
        _gray = material(Color(0.7f, 0.7f, 0.7f, 1.0f));

        // The chair: a compound of a seat, a back and four legs
        _chair = addEntity(root(), "Chair", Vector3(0.0f, 1.0f, 0.0f));
        addCollision(_chair, "compound");
        auto* chairBody = static_cast<RigidBodyComponent*>(_chair->addComponent<RigidBodyComponent>());
        chairBody->setType(RigidBodyType::Dynamic);
        chairBody->setFriction(0.5f);
        chairBody->setMass(10.0f);
        chairBody->setRestitution(0.5f);
        addBoxPart(_chair, "Seat", Vector3(0.0f, 0.0f, 0.0f), Vector3(0.25f, 0.025f, 0.25f));
        addBoxPart(_chair, "Seat Back", Vector3(0.0f, 0.3f, -0.2f), Vector3(0.25f, 0.2f, 0.025f));
        addLeg(_chair, "Leg 1", Vector3(0.2f, -0.25f, 0.2f), 0.5f);
        addLeg(_chair, "Leg 2", Vector3(-0.2f, -0.25f, 0.2f), 0.5f);
        addLeg(_chair, "Leg 3", Vector3(0.2f, 0.0f, -0.2f), 1.0f);
        addLeg(_chair, "Leg 4", Vector3(-0.2f, 0.0f, -0.2f), 1.0f);

        // The ground
        Entity* ground = addEntity(root(), "Ground", Vector3(0.0f, -0.5f, 0.0f));
        auto* groundCollision = addCollision(ground, "box");
        groundCollision->setHalfExtents(Vector3(5.0f, 0.5f, 5.0f));
        auto* groundBody = static_cast<RigidBodyComponent*>(ground->addComponent<RigidBodyComponent>());
        groundBody->setType(RigidBodyType::Static);
        groundBody->setRestitution(0.5f);
        addModel(ground, "Ground Model", "box", Vector3(10.0f, 1.0f, 10.0f));

        auto* light = createDirectionalLight(Vector3(45.0f, 130.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* component = light->findComponent<LightComponent>()) {
            component->setShadowDistance(8.0f);
            component->setShadowBias(0.1f);
            component->setShadowNormalBias(0.05f);
        }

        auto* camera = createCamera(Vector3(0.0f, 4.0f, 7.0f), Vector3(-30.0f, 0.0f, 0.0f));
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.5f, 0.5f, 0.5f, 1.0f));
        }
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        if (_time > 0.25f && _chairs < 20) {
            spawnChair();
            _time = 0.0f;
        }

        // Show the parts of active bodies in red and of sleeping ones in grey
        for (auto* body : engine()->components().instances<RigidBodyComponent>()) {
            if (!body || !body->entity()) {
                continue;
            }
            const bool active = body->type() == RigidBodyType::Static || body->isActive();
            for (auto* render : body->entity()->findComponents<RenderComponent>()) {
                render->setMaterial(active ? _red.get() : _gray.get());
            }
        }
    }

private:
    std::shared_ptr<StandardMaterial> material(const Color& color)
    {
        auto m = std::make_shared<StandardMaterial>();
        m->setDiffuse(color);
        _materials.push_back(m);
        return m;
    }

    Entity* addEntity(GraphNode* parent, const std::string& name, const Vector3& position)
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        entity->setName(name);
        entity->setLocalPosition(position);
        parent->addChild(entity);
        return entity;
    }

    static CollisionComponent* addCollision(Entity* entity, const std::string& type)
    {
        auto* collision = static_cast<CollisionComponent*>(entity->addComponent<CollisionComponent>());
        collision->setType(type);
        return collision;
    }

    // The render model of a part: a scaled primitive beneath it, so the part keeps unit scale
    void addModel(Entity* part, const std::string& name, const char* type, const Vector3& scale)
    {
        Entity* model = addEntity(part, name, Vector3(0.0f, 0.0f, 0.0f));
        model->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
        auto* render = static_cast<RenderComponent*>(model->addComponent<RenderComponent>());
        render->setMaterial(_gray.get());
        render->setType(type);
    }

    void addBoxPart(Entity* chair, const std::string& name, const Vector3& position, const Vector3& halfExtents)
    {
        Entity* part = addEntity(chair, name, position);
        addCollision(part, "box")->setHalfExtents(halfExtents);
        addModel(part, name + " Model", "box", halfExtents * 2.0f);
    }

    void addLeg(Entity* chair, const std::string& name, const Vector3& position, const float height)
    {
        Entity* leg = addEntity(chair, name, position);
        auto* collision = addCollision(leg, "cylinder");
        collision->setHeight(height);
        collision->setRadius(0.025f);
        addModel(leg, name + " Model", "cylinder", Vector3(0.05f, height, 0.05f));
    }

    void spawnChair()
    {
        Entity* clone = _chair->clone();
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const float x = unit(_rng) - 0.5f;
        const float y = unit(_rng) * 2.0f + 1.0f;
        const float z = unit(_rng) - 0.5f;
        clone->setLocalPosition(x, y, z);
        root()->addChild(clone);
        ++_chairs;
    }

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<StandardMaterial> _red;
    std::shared_ptr<StandardMaterial> _gray;
    Entity* _chair = nullptr;
    std::mt19937 _rng{20261013};
    float _time = 0.0f;
    int _chairs = 0;
};

VISUTWIN_EXAMPLE_MAIN(CompoundCollisionExample)
