// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream physics/offset-collision.
//
// The bitmoji character playing its idle clip on a 10 x 10 floor, with a STATIC rigid body
// whose collision is a 0.3 sphere moved 1.25 up by the collision's linear offset: the model's
// origin is at its feet, the sphere sits on its head. Forty balls (radius 0.2, mass 50,
// restitution 0.5) are dropped one every half second from 5 m, a few centimetres either side
// of the head, and bounce off it. An active ball is drawn red, a sleeping one grey, and the
// offset sphere is drawn as a green wire sphere at the collision's shape position. Helipad sky
// (mip 2, exposure 2), a shadowed directional light (PCF5, distance 12, 4096 map).
//
// DEVIATIONS:
// - The balls' x and z come from a fixed-seed generator rather than Math.random(), so every
//   run drops them the same way.
// - The wire sphere is three circles of one-pixel WideLines (16 segments each), standing in
//   for upstream's WireRenderer, which this port does not have.
//
#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include "../animClips.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/physics/jolt/joltPhysicsWorld.h"
#include "scene/constants.h"
#include "scene/graphics/wideLine.h"
#include "scene/graphics/wideLineRenderer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class OffsetCollisionExample final : public ExampleApp
{
public:
    OffsetCollisionExample() : ExampleApp({.title = "Offset Collision"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<CollisionComponentSystem>();
        options.registerComponentSystem<RigidBodyComponentSystem>();
        options.registerComponentSystem<AnimComponentSystem>();
        options.physicsWorld = createJoltPhysicsWorld();
    }

    bool create() override
    {
        _modelAsset = std::make_unique<Asset>("model", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _idleAsset = std::make_unique<Asset>("idleAnim", AssetType::CONTAINER, assetPath("animations/bitmoji/idle.glb"));
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});

        // The sky dome
        scene()->setExposure(2.0f);
        scene()->setSkyboxMip(2);
        if (Texture* helipad = _helipad->resourceAs<Texture>()) {
            scene()->setEnvAtlas(helipad);
        }

        auto* lightEntity = createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.5f, true);
        if (auto* light = lightEntity->findComponent<LightComponent>()) {
            light->setShadowNormalBias(0.2f);
            light->setShadowType(ShadowType::SHADOW_PCF5_32F);
            light->setShadowDistance(12.0f);
            light->setShadowResolution(4096);
            light->setShadowBias(0.2f);
        }

        _red = material(Color(1.0f, 0.3f, 0.3f, 1.0f));
        _gray = material(Color(0.7f, 0.7f, 0.7f, 1.0f));

        // The floor, its top at y = 0
        Entity* floor = createPrimitive("box", _gray.get(), Vector3(0.0f, -0.5f, 0.0f), Vector3(10.0f, 1.0f, 10.0f));
        auto* floorBody = static_cast<RigidBodyComponent*>(floor->addComponent<RigidBodyComponent>());
        floorBody->setType(RigidBodyType::Static);
        floorBody->setRestitution(0.5f);
        auto* floorCollision = static_cast<CollisionComponent*>(floor->addComponent<CollisionComponent>());
        floorCollision->setType("box");
        floorCollision->setHalfExtents(Vector3(5.0f, 0.5f, 5.0f));

        // The character, idling
        auto* container = _modelAsset->resourceAs<ContainerResource>();
        Entity* model = container ? container->instantiateRenderEntity() : nullptr;
        const auto idle = firstAnimTrack(_idleAsset.get());
        if (!model || !idle) {
            spdlog::error("offset-collision needs bitmoji.glb and its idle clip");
            return false;
        }
        model->setEngine(engine());
        AnimStateGraph stateGraph;
        auto& layer = stateGraph.addLayer("characterState");
        layer.states.push_back(AnimStateDesc{"Idle", 1.0f, true});
        layer.transitions.push_back(AnimTransitionDesc{.from = "START", .to = "Idle"});
        auto* anim = static_cast<AnimComponent*>(model->addComponent<AnimComponent>());
        anim->loadStateGraph(stateGraph);
        anim->baseLayer()->assignAnimation("Idle", idle);

        // A static body for the head, offset from the model's origin at its feet
        auto* modelBody = static_cast<RigidBodyComponent*>(model->addComponent<RigidBodyComponent>());
        modelBody->setType(RigidBodyType::Static);
        modelBody->setRestitution(0.5f);
        _head = static_cast<CollisionComponent*>(model->addComponent<CollisionComponent>());
        _head->setType("sphere");
        _head->setRadius(0.3f);
        _head->setLinearOffset(Vector3(0.0f, 1.25f, 0.0f));
        root()->addChild(model);

        auto* camera = createCamera(Vector3(0.0f, 2.0f, 5.0f));
        const Vector3 target = model->position();
        camera->lookAt(Vector3(target.getX(), target.getY() + 0.75f, target.getZ()));

        // The offset sphere's wire
        _wire = std::make_unique<WideLineRenderer>(engine(), device());
        _wire->setLayers({LAYERID_WORLD});
        const Vector3 center = _head->shapePosition();
        const Vector3 axes[3][2] = {
            {Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f)},
            {Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f)},
            {Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f)},
        };
        constexpr int kSegments = 16;
        for (const auto& [u, v] : axes) {
            std::vector<Vector3> points;
            for (int i = 0; i <= kSegments; ++i) {
                const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / kSegments;
                points.push_back(center + (u * std::cos(angle) + v * std::sin(angle)) * _head->radius());
            }
            auto line = std::make_unique<WideLine>();
            line->setPoints(points, Color(0.0f, 1.0f, 0.0f, 1.0f), 1.0f);
            _wire->add(line.get());
            _circles.push_back(std::move(line));
        }
        return true;
    }

    void update(const float dt) override
    {
        if (_remaining > 0) {
            _timer -= dt;
            if (_timer <= 0.0f) {
                --_remaining;
                _timer = 0.5f;
                dropBall();
            }
        }

        // Show active balls in red and sleeping ones in grey
        for (auto* ball : _balls) {
            if (auto* render = ball->entity()->findComponent<RenderComponent>()) {
                render->setMaterial(ball->isActive() ? _red.get() : _gray.get());
            }
        }

        const auto [width, height] = device()->size();
        _wire->setScreenSize(static_cast<float>(width), static_cast<float>(height));
        _wire->update();
    }

    void destroy() override
    {
        _wire.reset();
    }

private:
    std::shared_ptr<StandardMaterial> material(const Color& color)
    {
        auto m = std::make_shared<StandardMaterial>();
        m->setDiffuse(color);
        _materials.push_back(m);
        return m;
    }

    void dropBall()
    {
        std::uniform_real_distribution<float> offset(-0.25f, 0.25f);
        const Vector3 position(offset(_rng), 5.0f, offset(_rng));
        Entity* ball = createPrimitive("sphere", _red.get(), position, Vector3(0.4f, 0.4f, 0.4f));
        auto* body = static_cast<RigidBodyComponent*>(ball->addComponent<RigidBodyComponent>());
        body->setType(RigidBodyType::Dynamic);
        body->setMass(50.0f);
        body->setRestitution(0.5f);
        auto* collision = static_cast<CollisionComponent*>(ball->addComponent<CollisionComponent>());
        collision->setType("sphere");
        collision->setRadius(0.2f);
        _balls.push_back(body);
    }

    std::unique_ptr<Asset> _modelAsset;
    std::unique_ptr<Asset> _idleAsset;
    std::unique_ptr<Asset> _helipad;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<StandardMaterial> _red;
    std::shared_ptr<StandardMaterial> _gray;
    CollisionComponent* _head = nullptr;
    std::vector<RigidBodyComponent*> _balls;
    std::unique_ptr<WideLineRenderer> _wire;
    std::vector<std::unique_ptr<WideLine>> _circles;
    std::mt19937 _rng{20261013};
    float _timer = 0.0f;
    int _remaining = 40;
};

VISUTWIN_EXAMPLE_MAIN(OffsetCollisionExample)
