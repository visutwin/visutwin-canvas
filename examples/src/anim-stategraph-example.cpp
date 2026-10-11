// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Port of upstream's animation/locomotion example. Click the ground and the bitmoji
// character walks there (or jogs, with Run on), turning to face the way it goes, and
// stops on arrival. An anim state graph on the character picks the clip: an integer
// "speed" (0 still, 1 walk, 2 jog) moves it between Idle, Walk and Jog, and a "jump"
// trigger sends any state to Jump, which hands back to Idle at 80% of its clip. The
// character's speed across the ground follows the state, blended through a
// transition. The clips come from their own GLB files and bind to the rig by node name;
// the click finds the ground through a raycast against its static collider.
//
// Keys stand in for upstream's control panel:
//   Space  Jump (ignored while already jumping)
//   R      toggle Run: the next click jogs instead of walking
//
#include <cmath>
#include <memory>
#include <string>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/parsers/glbContainerResource.h"
#include "framework/physics/jolt/joltPhysicsWorld.h"
#include "platform/input/keyboard.h"
#include "platform/input/mouse.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    // The first animation track of a container (each bitmoji clip file holds one).
    std::shared_ptr<AnimTrack> firstAnimTrack(Asset* asset)
    {
        auto* glb = asset ? dynamic_cast<GlbContainerResource*>(asset->resourceAs<ContainerResource>()) : nullptr;
        if (!glb || glb->animTracks().empty()) {
            spdlog::error("Animation '{}' holds no animation track", asset ? asset->name() : "?");
            return nullptr;
        }
        return glb->animTracks().begin()->second;
    }

    // How many units a second the character covers in each state.
    float speedForState(const std::string& state)
    {
        if (state == "Walk") {
            return 1.0f;
        }
        if (state == "Jog") {
            return 4.0f;
        }
        return 0.0f;
    }
}

class AnimStateGraphExample final: public ExampleApp
{
public:
    AnimStateGraphExample(): ExampleApp({.title = "Locomotion"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
        options.registerComponentSystem<CollisionComponentSystem>();
        options.registerComponentSystem<RigidBodyComponentSystem>();
        options.physicsWorld = createJoltPhysicsWorld();
    }

    bool create() override
    {
        _groundTexture = std::make_unique<Asset>("playcanvasGreyTexture", AssetType::TEXTURE,
            assetPath("textures/playcanvas-grey.png"));
        _modelAsset = std::make_unique<Asset>("model", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _idleAsset = std::make_unique<Asset>("idleAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/idle.glb"));
        _walkAsset = std::make_unique<Asset>("walkAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/walk.glb"));
        _jogAsset = std::make_unique<Asset>("jogAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/run.glb"));
        _jumpAsset = std::make_unique<Asset>("jumpAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/jump-flip.glb"));
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});

        // The sky dome.
        scene()->setSkyboxMip(2);
        scene()->setSkyboxIntensity(0.7f);
        if (Texture* helipad = _helipad->resourceAs<Texture>()) {
            scene()->setEnvAtlas(helipad);
        }

        // translateLocal(0.5, 3, 8), then rotateLocal(-30, 0, 0).
        _camera = createCamera(Vector3(0.5f, 3.0f, 8.0f), Vector3(-30.0f, 0.0f, 0.0f));
        _camera->setName("Camera");
        if (auto* camera = _camera->findComponent<CameraComponent>(); camera && camera->camera()) {
            camera->camera()->setClearColor(Color(0.1f, 0.15f, 0.2f, 1.0f));
            camera->setToneMapping(TONEMAP_ACES);
        }

        auto* lightEntity = createDirectionalLight(Vector3(60.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 2.0f, true);
        if (auto* light = lightEntity->findComponent<LightComponent>()) {
            light->setShadowBias(0.2f);
            light->setShadowDistance(16.0f);
            light->setShadowNormalBias(0.05f);
            light->setShadowResolution(2048);
        }

        // The character entity carries the anim component and is moved by update();
        // the rendered model is its child, so the clips drive the model's nodes.
        auto* container = _modelAsset->resourceAs<ContainerResource>();
        Entity* renderEntity = container ? container->instantiateRenderEntity() : nullptr;
        if (!renderEntity) {
            spdlog::error("bitmoji.glb failed to load");
            return false;
        }
        _character = new Entity();
        _character->setEngine(engine());
        _character->addChild(renderEntity);

        const auto idle = firstAnimTrack(_idleAsset.get());
        const auto walk = firstAnimTrack(_walkAsset.get());
        const auto jog = firstAnimTrack(_jogAsset.get());
        const auto jump = firstAnimTrack(_jumpAsset.get());
        if (!idle || !walk || !jog || !jump) {
            return false;
        }

        AnimStateGraph stateGraph;
        stateGraph.addIntegerParameter("speed", 0);
        stateGraph.addTriggerParameter("jump");
        auto& layer = stateGraph.addLayer("locomotion");
        for (const char* state : {"Idle", "Walk", "Jump", "Jog"}) {
            layer.states.push_back(AnimStateDesc{state});
        }
        using P = AnimPredicate;
        layer.transitions = {
            {.from = "START", .to = "Idle"},
            {.from = "Idle", .to = "Walk", .time = 0.1f, .conditions = {{"speed", P::GREATER_THAN, 0.0f}}},
            {.from = "ANY", .to = "Jump", .time = 0.1f, .conditions = {{"jump", P::EQUAL_TO, 1.0f}}},
            {.from = "Jump", .to = "Idle", .time = 0.2f, .exitTime = 0.8f},
            {.from = "Jump", .to = "Walk", .time = 0.2f, .exitTime = 0.8f},
            {.from = "Walk", .to = "Idle", .time = 0.1f, .conditions = {{"speed", P::LESS_THAN_EQUAL_TO, 0.0f}}},
            {.from = "Walk", .to = "Jog", .time = 0.1f, .conditions = {{"speed", P::GREATER_THAN, 1.0f}}},
            {.from = "Jog", .to = "Walk", .time = 0.1f, .conditions = {{"speed", P::LESS_THAN, 2.0f}}},
        };

        _anim = static_cast<AnimComponent*>(_character->addComponent<AnimComponent>());
        _anim->loadStateGraph(stateGraph);
        _anim->assignAnimation("Idle", idle);
        _anim->assignAnimation("Walk", walk);
        _anim->assignAnimation("Jog", jog);
        _anim->assignAnimation("Jump", jump);
        root()->addChild(_character);

        // The ground: a 15 x 15 plane with a static, flat box collider for the click raycast.
        _groundMaterial = std::make_shared<StandardMaterial>();
        if (Texture* texture = _groundTexture->resourceAs<Texture>()) {
            _groundMaterial->setDiffuseMap(texture);
        }
        Entity* plane = createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(15.0f, 1.0f, 15.0f));
        plane->setName("Plane");
        auto* collision = static_cast<CollisionComponent*>(plane->addComponent<CollisionComponent>());
        collision->setType("box");
        collision->setHalfExtents(Vector3(7.5f, 0.0f, 7.5f));
        auto* body = static_cast<RigidBodyComponent*>(plane->addComponent<RigidBodyComponent>());
        body->setType(RigidBodyType::Static);

        _rigidBodies = dynamic_cast<RigidBodyComponentSystem*>(engine()->systems()->getById("rigidbody"));

        spdlog::info("Click the ground to walk there; Space jumps, R toggles Run (now off).");
        return true;
    }

    void update(const float dt) override
    {
        if (!_anim || !_anim->baseLayer()) {
            return;
        }
        AnimComponentLayer* layer = _anim->baseLayer();

        if (const auto* keyboard = engine()->keyboard()) {
            if (keyboard->wasPressed(Key::Space) && layer->activeState() != "Jump") {
                _anim->setTrigger("jump");
            }
            if (keyboard->wasPressed(Key::R)) {
                _jogToggle = !_jogToggle;
                spdlog::info("Run {}", _jogToggle ? "on" : "off");
            }
        }

        // A left click sets the target to the point of the ground under the pointer.
        if (const auto* mouse = engine()->mouse(); mouse && mouse->wasPressed(MouseButton::Left) && _rigidBodies) {
            if (auto* camera = _camera->findComponent<CameraComponent>(); camera && camera->camera()) {
                const Vector3 nearPoint = camera->screenToWorld(mouse->x(), mouse->y(), camera->camera()->nearClip());
                const Vector3 farPoint = camera->screenToWorld(mouse->x(), mouse->y(), camera->camera()->farClip());
                if (const auto result = _rigidBodies->raycastFirst(farPoint, nearPoint)) {
                    _targetPosition = Vector3(result->point.getX(), 0.0f, result->point.getZ());
                    _anim->setInteger("speed", _jogToggle ? 2 : 1);
                }
            }
        }

        if (_anim->getInteger("speed") == 0) {
            return;
        }
        // Move toward the target at the state's speed, blended across a transition, facing
        // the way the character goes.
        float moveSpeed = speedForState(layer->activeState());
        if (layer->transitioning()) {
            const float progress = layer->transitionProgress();
            moveSpeed = speedForState(layer->previousState()) * (1.0f - progress) + moveSpeed * progress;
        }
        const Vector3 distance = _targetPosition - _currentPosition;
        const Vector3 direction = distance.normalized();
        const Vector3 movement = direction * (dt * moveSpeed);
        if (movement.length() < distance.length()) {
            _currentPosition = _currentPosition + movement;
            _character->setPosition(_currentPosition);
            _character->lookAt(_currentPosition - direction);
        } else {
            _currentPosition = _targetPosition;
            _character->setPosition(_currentPosition);
            _anim->setInteger("speed", 0);
        }
    }

private:
    std::unique_ptr<Asset> _groundTexture;
    std::unique_ptr<Asset> _modelAsset;
    std::unique_ptr<Asset> _idleAsset;
    std::unique_ptr<Asset> _walkAsset;
    std::unique_ptr<Asset> _jogAsset;
    std::unique_ptr<Asset> _jumpAsset;
    std::unique_ptr<Asset> _helipad;

    std::shared_ptr<StandardMaterial> _groundMaterial;
    Entity* _camera = nullptr;
    Entity* _character = nullptr;
    AnimComponent* _anim = nullptr;
    RigidBodyComponentSystem* _rigidBodies = nullptr;

    bool _jogToggle = false;
    Vector3 _targetPosition{2.0f, 0.0f, 2.0f};
    Vector3 _currentPosition{0.0f, 0.0f, 0.0f};
};

VISUTWIN_EXAMPLE_MAIN(AnimStateGraphExample)
