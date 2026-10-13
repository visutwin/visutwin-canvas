// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream physics/mesh-collider-scaling.
//
// Four static tori lie flat on a 24 x 1 x 12 grey static floor (restitution 0.5), each a
// rigid body (restitution 0.4) with a `mesh` collision volume built from ONE shared torus
// mesh (ring radius 1, tube radius 0.25), which each collider takes at its own entity's
// world scale. Three keep fixed scales, 0.5, 1 and 1.5 at x -6, -2 and 2; the fourth, at
// x 6, pulses between 0.5 and 1.5 (1 + 0.5 sin t), raised by 0.25 x scale so it stays on
// the floor, and its collider is rebuilt every frame from the new scale. Every quarter
// second, up to a hundred, a red sphere (radius 0.2, mass 1, restitution 0.4) is dropped
// from height 6 over a random torus, at a random angle and a distance of 0.5 - 1.2 times
// the torus's scale from its centre: spheres over the ring come to rest on it, spheres over
// the hole fall through, at every scale. Ambient 0.2, a directional light at (45, 30, 0)
// casting 2048 shadows (bias 0.2, normal offset 0.05, distance 40), and a camera at
// (0, 9, 14) looking at (0, 0.5, 0), clearing to (0.5, 0.5, 0.8), far clip 100.
//
// DEVIATIONS:
// - upstream's torus comes from torus.glb, which is not among the shipped assets, so it is
//   generated here at the glb's size and tessellation (ring radius 1, tube radius 0.25, 48
//   segments around the ring, 12 around the tube), with the glTF default material the glb
//   gets (white, metalness 1, gloss 0).
// - a static body does not follow its entity, so the pulsing torus's body is rebuilt
//   explicitly after each scale change (upstream rebuilds a mesh collider automatically
//   when its entity's scale changes).
// - the sphere template is parented to the root, disabled, where upstream keeps it out of
//   the hierarchy; a disabled entity is neither drawn nor simulated either way.
//
#include <cmath>
#include <memory>
#include <numbers>
#include <random>
#include <utility>
#include <vector>

#include "../exampleApp.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/physics/jolt/joltPhysicsWorld.h"
#include "framework/physics/physicsWorld.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

class MeshColliderScalingExample final: public ExampleApp
{
public:
    MeshColliderScalingExample(): ExampleApp({.title = "Mesh Collider Scaling"}) {}

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

        // The gravity for the rigid bodies.
        if (PhysicsWorld* world = engine()->physicsWorld()) {
            world->setGravity(Vector3(0.0f, -9.81f, 0.0f));
        }

        // The floor.
        Entity* floor = createPrimitive("box", createMaterial(Color(0.7f, 0.7f, 0.7f, 1.0f)),
            Vector3(0.0f, -0.5f, 0.0f), Vector3(24.0f, 1.0f, 12.0f));
        auto* floorBody = static_cast<RigidBodyComponent*>(floor->addComponent<RigidBodyComponent>());
        floorBody->setType(RigidBodyType::Static);
        floorBody->setRestitution(0.5f);
        auto* floorCollision = static_cast<CollisionComponent*>(floor->addComponent<CollisionComponent>());
        floorCollision->setType("box");
        floorCollision->setHalfExtents(Vector3(12.0f, 0.5f, 6.0f));

        // The light.
        auto* light = createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComponent = light->findComponent<LightComponent>()) {
            lightComponent->setShadowBias(0.2f);
            lightComponent->setShadowDistance(40.0f);
            lightComponent->setShadowNormalBias(0.05f);
            lightComponent->setShadowResolution(2048);
        }

        // The camera.
        auto* camera = createCamera(Vector3(0.0f, 9.0f, 14.0f));
        if (auto* cameraComponent = camera->findComponent<CameraComponent>();
            cameraComponent && cameraComponent->camera()) {
            cameraComponent->camera()->setClearColor(Color(0.5f, 0.5f, 0.8f, 1.0f));
            cameraComponent->camera()->setFarClip(100.0f);
        }
        camera->lookAt(0.0f, 0.5f, 0.0f);

        // A single torus mesh is used for every mesh collider below. The torus lies flat:
        // its ring runs at a radius of 1 around the local Y axis, with a tube radius of
        // 0.25. Each collider applies its own entity scale to it.
        _torusMesh = createMeshFromGeometry(device(), createTorusGeometry(0.25f, 1.0f, 360.0f, 48, 12));
        _torusMaterial = std::make_shared<StandardMaterial>();
        _torusMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _torusMaterial->setUseMetalness(true);
        _torusMaterial->setMetalness(1.0f);
        _torusMaterial->setGloss(0.0f);

        // Three tori at fixed scales. Spheres dropped over a ring come to rest on it,
        // spheres dropped over the hole fall through, at every scale, because the
        // collision mesh follows the entity.
        _tori = {createTorus(-6.0f, 0.5f), createTorus(-2.0f, 1.0f), createTorus(2.0f, 1.5f)};

        // ...and one whose scale is animated every frame.
        _pulsing = createTorus(6.0f, 1.0f);
        _tori.push_back(_pulsing);

        // The sphere template.
        _sphereTemplate = createPrimitive("sphere", createMaterial(Color(1.0f, 0.3f, 0.3f, 1.0f)),
            Vector3(0.0f, 0.0f, 0.0f), Vector3(0.4f, 0.4f, 0.4f));
        auto* sphereBody = static_cast<RigidBodyComponent*>(_sphereTemplate->addComponent<RigidBodyComponent>());
        sphereBody->setType(RigidBodyType::Dynamic);
        sphereBody->setMass(1.0f);
        sphereBody->setRestitution(0.4f);
        auto* sphereCollision = static_cast<CollisionComponent*>(_sphereTemplate->addComponent<CollisionComponent>());
        sphereCollision->setType("sphere");
        sphereCollision->setRadius(0.2f);
        _sphereTemplate->setEnabled(false);

        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Pulse the fourth torus between half and one and a half times its size, keeping
        // it on the floor. Its static body is rebuilt from the new scale.
        const float scale = 1.0f + 0.5f * std::sin(_time);
        _pulsing->setLocalScale(scale, scale, scale);
        _pulsing->setLocalPosition(6.0f, 0.25f * scale, 0.0f);
        if (auto* body = _pulsing->findComponent<RigidBodyComponent>()) {
            body->rebuildBody();
        }

        // Drop a sphere over a random torus every quarter second, somewhere around its ring.
        _spawnTimer -= dt;
        if (_spawned < 100 && _spawnTimer <= 0.0f) {
            _spawnTimer = 0.25f;
            ++_spawned;

            std::uniform_int_distribution<size_t> pick(0, _tori.size() - 1);
            std::uniform_real_distribution<float> distance(0.5f, 1.2f);
            std::uniform_real_distribution<float> turn(0.0f, 2.0f * std::numbers::pi_v<float>);

            Entity* torus = _tori[pick(_rng)];
            const Vector3 position = torus->position();
            const float radius = torus->localScale().getX() * distance(_rng);
            const float angle = turn(_rng);

            Entity* sphere = _sphereTemplate->clone();
            sphere->setEnabled(true);
            root()->addChild(sphere);
            if (auto* body = sphere->findComponent<RigidBodyComponent>()) {
                body->teleport(Vector3(position.getX() + std::cos(angle) * radius, 6.0f,
                    position.getZ() + std::sin(angle) * radius));
            }
        }
    }

private:
    Material* createMaterial(const Color& color)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        _materials.push_back(material);
        return material.get();
    }

    /// A static torus with a mesh collider that follows the entity scale.
    Entity* createTorus(const float x, const float scale)
    {
        auto* torus = new Entity();
        torus->setEngine(engine());
        auto* render = static_cast<RenderComponent*>(torus->addComponent<RenderComponent>());
        auto meshInstance = std::make_unique<MeshInstance>(_torusMesh, _torusMaterial, torus);
        meshInstance->setCastShadow(true);
        render->setCastShadows(true);
        render->addMeshInstance(std::move(meshInstance));
        torus->setLocalScale(scale, scale, scale);
        torus->setLocalPosition(x, 0.25f * scale, 0.0f);

        auto* body = static_cast<RigidBodyComponent*>(torus->addComponent<RigidBodyComponent>());
        body->setType(RigidBodyType::Static);
        body->setRestitution(0.4f);
        auto* collision = static_cast<CollisionComponent*>(torus->addComponent<CollisionComponent>());
        collision->setType("mesh");
        collision->setRender({_torusMesh});

        root()->addChild(torus);
        return torus;
    }

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<StandardMaterial> _torusMaterial;
    std::shared_ptr<Mesh> _torusMesh;
    std::vector<Entity*> _tori;
    Entity* _pulsing = nullptr;
    Entity* _sphereTemplate = nullptr;
    std::mt19937 _rng{20261013};
    float _time = 0.0f;
    float _spawnTimer = 0.0f;
    int _spawned = 0;
};

VISUTWIN_EXAMPLE_MAIN(MeshColliderScalingExample)
