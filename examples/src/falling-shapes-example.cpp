// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream physics/falling-shapes.
//
// A grey floor with restitution 0.5, and forty dynamic shapes dropped onto it one
// every 0.2 seconds from ten metres up, each a random pick from box, sphere,
// capsule and cylinder.
//
// The two lines that matter are in configure(): the engine owns no simulation, so
// an application supplies a PhysicsWorld through AppOptions, the same way it
// supplies component systems.
//
// DEVIATION: upstream's fifth shape is a torus rendered from a glTF container with
// a `mesh` collision volume built from the render asset. This port has no mesh
// collision shape, so the rotation is over the four primitives it does have.
//
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/physics/jolt/joltPhysicsWorld.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class FallingShapesExample final: public ExampleApp
{
public:
    FallingShapesExample(): ExampleApp({.title = "Falling Shapes"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<CollisionComponentSystem>();
        options.registerComponentSystem<RigidBodyComponentSystem>();
        options.physicsWorld = createJoltPhysicsWorld();
    }

    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        _red = material(Color(1.0f, 0.3f, 0.3f, 1.0f));
        _gray = material(Color(0.7f, 0.7f, 0.7f, 1.0f));

        // Floor: a 10 x 1 x 10 static box that other bodies bounce off.
        {
            Entity* floor = createPrimitive("box", _gray.get(), Vector3(0.0f, 0.0f, 0.0f),
                Vector3(10.0f, 1.0f, 10.0f));
            auto* collision = static_cast<CollisionComponent*>(
                floor->addComponent<CollisionComponent>());
            collision->setType("box");
            collision->setHalfExtents(Vector3(5.0f, 0.5f, 5.0f));
            auto* body = static_cast<RigidBodyComponent*>(
                floor->addComponent<RigidBodyComponent>());
            body->setType(RigidBodyType::Static);
            body->setRestitution(0.5f);
        }

        auto* light = createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowDistance(25.0f);
            lightComp->setShadowNormalBias(0.05f);
            lightComp->setShadowResolution(2048);
        }

        auto* camera = createCamera(Vector3(0.0f, 10.0f, 15.0f));
        if (auto* comp = camera->findComponent<CameraComponent>();
            comp != nullptr && comp->camera() != nullptr) {
            comp->camera()->setFarClip(50.0f);
            comp->camera()->setClearColor(Color(0.5f, 0.5f, 0.8f, 1.0f));
        }
        auto* controls = addOrbitControls(camera, Vector3(0.0f, 2.0f, 0.0f));
        controls->setOrbitDistance(18.0f);
        controls->storeResetState();

        spdlog::info("Dropping {} shapes, one every {:.1f} s", _remaining, kInterval);
        return true;
    }

    void update(const float dt) override
    {
        if (_remaining <= 0) {
            return;
        }
        _timer -= dt;
        if (_timer > 0.0f) {
            return;
        }
        _timer = kInterval;
        --_remaining;
        spawn();
    }

private:
    static constexpr float kInterval = 0.2f;

    std::shared_ptr<StandardMaterial> material(const Color& color)
    {
        auto m = std::make_shared<StandardMaterial>();
        m->setDiffuse(color);
        _materials.push_back(m);
        return m;
    }

    void spawn()
    {
        std::uniform_real_distribution<float> offset(-1.0f, 1.0f);
        std::uniform_int_distribution<int> pick(0, 3);
        const Vector3 position(offset(_rng), 10.0f, offset(_rng));

        const char* shapes[] = {"box", "sphere", "capsule", "cylinder"};
        const int which = pick(_rng);
        const char* shape = shapes[which];

        Vector3 scale(1.0f, 1.0f, 1.0f);
        if (which == 2) {          // capsule: radius 0.5, height 2
            scale = Vector3(1.0f, 2.0f, 1.0f);
        } else if (which == 3) {   // cylinder: radius 0.5, height 1
            scale = Vector3(1.0f, 1.0f, 1.0f);
        }

        Entity* entity = createPrimitive(shape, _red.get(), position, scale);
        auto* collision = static_cast<CollisionComponent*>(
            entity->addComponent<CollisionComponent>());
        collision->setType(shape);
        collision->setHalfExtents(Vector3(0.5f, 0.5f, 0.5f));
        collision->setRadius(0.5f);
        collision->setHeight(which == 2 ? 2.0f : 1.0f);

        auto* body = static_cast<RigidBodyComponent*>(
            entity->addComponent<RigidBodyComponent>());
        body->setType(RigidBodyType::Dynamic);
        body->setMass(50.0f);
        body->setRestitution(0.5f);
    }

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<StandardMaterial> _red, _gray;
    std::mt19937 _rng{20260905};
    float _timer = 0.0f;
    int _remaining = 40;
};

VISUTWIN_EXAMPLE_MAIN(FallingShapesExample)
