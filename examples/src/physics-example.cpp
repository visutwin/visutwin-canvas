// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Rigid-body simulation through the PhysicsWorld seam.
//
// The engine owns no simulation. It declares PhysicsWorld and PhysicsBody, and an
// application hands one in through AppOptions — the same rule component systems
// follow. Two lines in configure() are what separate a scene that falls over from
// one that stands frozen:
//
//     options.registerComponentSystem<RigidBodyComponentSystem>();
//     options.physicsWorld = createJoltPhysicsWorld();
//
// The scene is a pyramid of boxes on a static floor. Space drops a heavy sphere
// onto it from above, R rebuilds the stack, and clicking fires a raycast into the
// scene and pushes whatever it hits.
//
#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/physics/jolt/joltPhysicsWorld.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kBoxHalf = 0.25f;
    constexpr int kPyramidRows = 5;
}

class PhysicsExample final: public ExampleApp
{
public:
    PhysicsExample(): ExampleApp({.title = "Physics"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<CollisionComponentSystem>();
        options.registerComponentSystem<RigidBodyComponentSystem>();
        // Without a world the components are inert: they hold their settings and
        // nothing ever moves.
        options.physicsWorld = createJoltPhysicsWorld();
    }

    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.12f, 0.13f, 0.16f);

        _envAtlas = std::make_unique<Asset>(
            "helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (const auto env = _envAtlas->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*env));
        }

        auto* light = createDirectionalLight(Vector3(48.0f, 25.0f, 0.0f),
            Color(1.0f, 0.96f, 0.9f, 1.0f), 2.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowResolution(2048);
            lightComp->setShadowDistance(40.0f);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
        }

        _floorMaterial = std::make_shared<StandardMaterial>();
        _floorMaterial->setDiffuse(Color(0.22f, 0.24f, 0.26f, 1.0f));
        _floorMaterial->setMetalness(0.0f);
        _floorMaterial->setGloss(0.35f);

        // The floor. A static box rather than a plane primitive, so its collision
        // shape and its mesh describe the same slab.
        {
            Entity* floor = createPrimitive("box", _floorMaterial.get(),
                Vector3(0.0f, -0.5f, 0.0f), Vector3(24.0f, 1.0f, 24.0f));
            addBody(floor, RigidBodyType::Static, Vector3(12.0f, 0.5f, 12.0f), 0.0f);
        }

        buildPyramid();

        auto* camera = createCamera(Vector3(4.5f, 3.0f, 7.0f));
        if (auto* comp = camera->findComponent<CameraComponent>();
            comp != nullptr && comp->camera() != nullptr) {
            comp->camera()->setNearClip(0.1f);
            comp->camera()->setFarClip(120.0f);
        }
        _controls = addOrbitControls(camera, Vector3(0.0f, 1.0f, 0.0f));
        _controls->setOrbitDistance(9.0f);
        _controls->storeResetState();
        _camera = camera;

        spdlog::info("Space drops a heavy sphere, R rebuilds the stack, "
                     "A toggles the auto-demo (on by default).");
        return true;
    }

    void update(const float dt) override
    {
        if (!_autoDemo) {
            return;
        }
        _demoTimer += dt;
        // Drop one sphere every couple of seconds and rebuild once the stack has
        // been thoroughly knocked over, so the example demonstrates itself.
        if (_demoTimer > 2.0f) {
            _demoTimer = 0.0f;
            if (++_dropped > 4) {
                _dropped = 0;
                rebuild();
            } else {
                dropSphere();
            }
        }
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_SPACE) {
                dropSphere();
                return true;
            }
            if (event.key.key == SDLK_R) {
                rebuild();
                return true;
            }
            if (event.key.key == SDLK_A) {
                _autoDemo = !_autoDemo;
                spdlog::info("[auto-demo] {}", _autoDemo ? "ON" : "OFF");
                return true;
            }
        }
        return false;
    }

private:
    RigidBodyComponent* addBody(Entity* entity, const RigidBodyType type,
        const Vector3& halfExtents, const float mass, const bool sphere = false,
        const float restitution = 0.1f)
    {
        auto* collision = static_cast<CollisionComponent*>(
            entity->addComponent<CollisionComponent>());
        if (collision == nullptr) {
            return nullptr;
        }
        if (sphere) {
            collision->setType("sphere");
            collision->setRadius(halfExtents.getX());
        } else {
            collision->setType("box");
            collision->setHalfExtents(halfExtents);
        }

        auto* body = static_cast<RigidBodyComponent*>(
            entity->addComponent<RigidBodyComponent>());
        if (body == nullptr) {
            return nullptr;
        }
        body->setType(type);
        body->setMass(mass);
        body->setFriction(0.6f);
        body->setRestitution(restitution);
        // A little damping keeps a settled stack from jittering forever.
        body->setLinearDamping(0.05f);
        body->setAngularDamping(0.1f);
        return body;
    }

    void buildPyramid()
    {
        static const Color colors[] = {
            Color(0.85f, 0.35f, 0.30f, 1.0f), Color(0.35f, 0.65f, 0.85f, 1.0f),
            Color(0.90f, 0.72f, 0.30f, 1.0f), Color(0.45f, 0.78f, 0.45f, 1.0f),
            Color(0.72f, 0.45f, 0.85f, 1.0f),
        };

        int index = 0;
        for (int row = 0; row < kPyramidRows; ++row) {
            const int count = kPyramidRows - row;
            for (int i = 0; i < count; ++i) {
                auto material = std::make_shared<StandardMaterial>();
                material->setDiffuse(colors[index % 5]);
                material->setMetalness(0.0f);
                material->setGloss(0.45f);
                _materials.push_back(material);

                const float x = (static_cast<float>(i) - static_cast<float>(count - 1) * 0.5f)
                    * (kBoxHalf * 2.2f);
                const float y = kBoxHalf + static_cast<float>(row) * kBoxHalf * 2.05f;
                Entity* box = createPrimitive("box", material.get(),
                    Vector3(x, y, 0.0f),
                    Vector3(kBoxHalf * 2.0f, kBoxHalf * 2.0f, kBoxHalf * 2.0f));
                addBody(box, RigidBodyType::Dynamic,
                    Vector3(kBoxHalf, kBoxHalf, kBoxHalf), 1.0f);
                _stack.push_back(box);
                ++index;
            }
        }
    }

    void dropSphere()
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(0.92f, 0.92f, 0.95f, 1.0f));
        material->setMetalness(0.9f);
        material->setGloss(0.8f);
        _materials.push_back(material);

        std::uniform_real_distribution<float> jitter(-0.25f, 0.25f);
        const float radius = 0.4f;
        Entity* ball = createPrimitive("sphere", material.get(),
            Vector3(jitter(_rng), 6.0f, jitter(_rng)),
            Vector3(radius * 2.0f, radius * 2.0f, radius * 2.0f));
        addBody(ball, RigidBodyType::Dynamic, Vector3(radius, radius, radius), 12.0f, true, 0.2f);
        _stack.push_back(ball);
        spdlog::info("Dropped a sphere; {} bodies in the scene", _stack.size());
    }

    void rebuild()
    {
        for (Entity* entity : _stack) {
            if (entity && entity->parent()) {
                // removeChild hands back ownership; letting it fall out of scope
                // destroys the entity, which is what releases its physics body.
                const auto owned = entity->parent()->removeChild(entity);
            }
        }
        _stack.clear();
        buildPyramid();
        spdlog::info("Stack rebuilt");
    }

    std::unique_ptr<Asset> _envAtlas;
    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Entity*> _stack;
    CameraControls* _controls = nullptr;
    Entity* _camera = nullptr;
    std::mt19937 _rng{1337};
    float _demoTimer = 0.0f;
    int _dropped = 0;
    bool _autoDemo = true;
};

VISUTWIN_EXAMPLE_MAIN(PhysicsExample)
