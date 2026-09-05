// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream physics/joints.
//
// Six mechanisms in a row, each built from one joint type:
//   * a door on a limited hinge
//   * a windmill rotor on a motorised hinge
//   * a hanging chain of ball joints with asymmetric swing limits
//   * a platform patrolling a rail on a motorised slider, reversed every 3 s
//   * a crate bobbing under a six-degree-of-freedom spring
//   * a tower of boxes welded with breakable fixed joints, tinted red as they part
//
// A joint lives on its OWN entity, and that entity's transform is the joint frame:
// its local X axis is the primary axis. The entity is positioned and parented
// BEFORE the component is added, so the frame is captured from the final world
// transform — the same ordering rule upstream documents.
//
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/joint/jointComponent.h"
#include "framework/components/joint/jointComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/physics/jolt/joltPhysicsWorld.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class PhysicsJointsExample final: public ExampleApp
{
public:
    PhysicsJointsExample(): ExampleApp({.title = "Physics Joints"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<CollisionComponentSystem>();
        options.registerComponentSystem<RigidBodyComponentSystem>();
        options.registerComponentSystem<JointComponentSystem>();
        options.physicsWorld = createJoltPhysicsWorld();
    }

    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        _gray = material(Color(0.7f, 0.7f, 0.7f, 1.0f));
        _wood = material(Color(0.6f, 0.4f, 0.2f, 1.0f));
        _blue = material(Color(0.3f, 0.5f, 0.9f, 1.0f));
        _green = material(Color(0.3f, 0.8f, 0.4f, 1.0f));
        _red = material(Color(1.0f, 0.3f, 0.3f, 1.0f));

        // Floor, light and camera, at upstream's poses.
        Entity* floor = createBox("floor", Vector3(26.0f, 1.0f, 10.0f),
            Vector3(0.0f, 0.0f, 0.0f), _gray.get(), RigidBodyType::Static, 0.0f);
        if (auto* body = floor->findComponent<RigidBodyComponent>()) {
            body->setRestitution(0.4f);
        }

        auto* light = createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowDistance(40.0f);
            lightComp->setShadowNormalBias(0.05f);
            lightComp->setShadowResolution(2048);
        }

        auto* camera = createCamera(Vector3(0.0f, 6.0f, 18.0f));
        if (auto* comp = camera->findComponent<CameraComponent>();
            comp != nullptr && comp->camera() != nullptr) {
            comp->camera()->setFarClip(100.0f);
            comp->camera()->setClearColor(Color(0.5f, 0.5f, 0.8f, 1.0f));
        }
        _camera = camera;
        auto* controls = addOrbitControls(camera, Vector3(0.0f, 2.0f, 0.0f));
        controls->setOrbitDistance(19.0f);
        controls->storeResetState();

        buildDoor();
        buildWindmill();
        buildChain();
        buildSlider();
        buildSpringCrate();
        buildBreakableTower();

        spdlog::info("Joints: hinge door, motorised windmill, ball chain, sliding "
                     "platform, sprung crate, breakable welds. Click to shoot a ball.");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.button == SDL_BUTTON_LEFT) {
            shootBall();
            return false;   // the orbit controls still want this event
        }
        return false;
    }

    void update(const float dt) override
    {
        // Reverse the slider motor at each end of the rail, as upstream does.
        // A body only exists after its own first update, so the launch velocity is
        // applied on the frame AFTER the ball is created.
        for (auto it = _pendingShots.begin(); it != _pendingShots.end();) {
            if (it->body != nullptr && it->body->physicsBody() != nullptr) {
                it->body->setLinearVelocity(it->velocity);
                it = _pendingShots.erase(it);
            } else {
                ++it;
            }
        }

        _patrolTimer += dt;
        if (_patrolTimer > 3.0f && _sliderJoint != nullptr) {
            _patrolTimer = 0.0f;
            _sliderJoint->setMotorSpeed(-_sliderJoint->motorSpeed());
        }
    }

private:
    /// Upstream fires the ball through the cursor with Camera.screenToWorld;
    /// this engine has no such helper, so the ball goes along the camera's forward
    /// axis instead. DEVIATION, and it only differs for an off-centre click.
    void shootBall()
    {
        if (_camera == nullptr) {
            return;
        }
        const Matrix4 world = _camera->worldTransform();
        const Vector3 forward = Vector3(-world.getElement(2, 0), -world.getElement(2, 1),
            -world.getElement(2, 2)).normalized();
        const Vector3 origin = _camera->position() + forward * 1.0f;

        Entity* ball = createPrimitive("sphere", _red.get(), origin,
            Vector3(0.5f, 0.5f, 0.5f));
        auto* collision = static_cast<CollisionComponent*>(
            ball->addComponent<CollisionComponent>());
        collision->setType("sphere");
        collision->setRadius(0.25f);
        auto* body = static_cast<RigidBodyComponent*>(
            ball->addComponent<RigidBodyComponent>());
        body->setType(RigidBodyType::Dynamic);
        body->setMass(15.0f);
        _pendingShots.push_back({body, forward * 35.0f});
    }

    std::shared_ptr<StandardMaterial> material(const Color& color)
    {
        auto m = std::make_shared<StandardMaterial>();
        m->setDiffuse(color);
        _materials.push_back(m);
        return m;
    }

    Entity* createBox(const char* name, const Vector3& size, const Vector3& position,
        Material* mat, const RigidBodyType type, const float mass)
    {
        Entity* entity = createPrimitive("box", mat, position, size);
        entity->setName(name);

        auto* collision = static_cast<CollisionComponent*>(
            entity->addComponent<CollisionComponent>());
        collision->setType("box");
        collision->setHalfExtents(Vector3(size.getX() * 0.5f, size.getY() * 0.5f,
                                          size.getZ() * 0.5f));

        auto* body = static_cast<RigidBodyComponent*>(
            entity->addComponent<RigidBodyComponent>());
        body->setType(type);
        body->setMass(mass);
        return entity;
    }

    /// A joint entity: positioned and parented BEFORE the component is added, so
    /// the frame is taken from the final world transform.
    JointComponent* createJoint(const char* name, const Vector3& position,
        const Vector3& eulerAngles)
    {
        auto* entity = new Entity();
        entity->setName(name);
        entity->setEngine(engine());
        entity->setLocalPosition(position);
        entity->setLocalEulerAngles(eulerAngles.getX(), eulerAngles.getY(), eulerAngles.getZ());
        root()->addChild(entity);
        return static_cast<JointComponent*>(entity->addComponent<JointComponent>());
    }

    void buildDoor()
    {
        Entity* frame = createBox("door-frame", Vector3(0.25f, 3.0f, 0.25f),
            Vector3(-10.5f, 2.0f, 0.0f), _wood.get(), RigidBodyType::Static, 0.0f);
        Entity* door = createBox("door", Vector3(1.6f, 2.4f, 0.12f),
            Vector3(-9.55f, 1.95f, 0.0f), _green.get(), RigidBodyType::Dynamic, 20.0f);

        // The hinge sits at the frame with its X axis pointing up; the door swings
        // through 110 degrees.
        auto* joint = createJoint("door-hinge", Vector3(-10.35f, 1.95f, 0.0f),
            Vector3(0.0f, 0.0f, 90.0f));
        joint->setType(PhysicsJointType::Hinge);
        joint->setEntityA(door);
        joint->setEntityB(frame);
        joint->setLimits(0.0f, 110.0f);
    }

    void buildWindmill()
    {
        Entity* pole = createBox("pole", Vector3(0.25f, 4.0f, 0.25f),
            Vector3(-5.5f, 2.0f, 0.0f), _wood.get(), RigidBodyType::Static, 0.0f);
        Entity* rotor = createBox("rotor", Vector3(3.5f, 0.35f, 0.15f),
            Vector3(-5.5f, 4.0f, 0.3f), _blue.get(), RigidBodyType::Dynamic, 10.0f);

        // Hinge axis toward the camera, spun at a constant angular speed.
        auto* joint = createJoint("windmill-hinge", Vector3(-5.5f, 4.0f, 0.3f),
            Vector3(0.0f, -90.0f, 0.0f));
        joint->setType(PhysicsJointType::Hinge);
        joint->setEntityA(rotor);
        joint->setEntityB(pole);
        joint->setMotorSpeed(90.0f);
        joint->setMaxMotorForce(100.0f);
    }

    void buildChain()
    {
        std::vector<Entity*> links;
        for (int i = 0; i < 6; ++i) {
            Entity* link = createBox(("link-" + std::to_string(i)).c_str(),
                Vector3(0.18f, 0.5f, 0.18f),
                Vector3(-2.0f, 5.25f - static_cast<float>(i) * 0.5f, 0.0f),
                _blue.get(), RigidBodyType::Dynamic, 2.0f);
            links.push_back(link);

            // Each ball joint sits at the top of its link with X down the chain
            // and Y along world X: wide swing left and right, almost none forward.
            auto* joint = createJoint(("chain-joint-" + std::to_string(i)).c_str(),
                Vector3(-2.0f, 5.5f - static_cast<float>(i) * 0.5f, 0.0f),
                Vector3(0.0f, 0.0f, -90.0f));
            joint->setType(PhysicsJointType::Ball);
            joint->setEntityA(link);
            joint->setEntityB(i > 0 ? links[i - 1] : nullptr);
            joint->setSwingLimits(60.0f, 5.0f);
            joint->setTwistLimit(10.0f);
        }

        _chainTail = links.back();
    }

    void buildSlider()
    {
        // The rail is a visual guide only, with no body.
        createPrimitive("box", _wood.get(), Vector3(1.5f, 0.62f, 0.0f),
            Vector3(4.5f, 0.08f, 0.5f));

        Entity* platform = createBox("platform", Vector3(0.8f, 0.25f, 0.6f),
            Vector3(1.5f, 0.85f, 0.0f), _green.get(), RigidBodyType::Dynamic, 10.0f);

        // Pinned to the world: the platform only translates along the frame's X.
        _sliderJoint = createJoint("slider", Vector3(1.5f, 0.85f, 0.0f),
            Vector3(0.0f, 0.0f, 0.0f));
        _sliderJoint->setType(PhysicsJointType::Slider);
        _sliderJoint->setEntityA(platform);
        _sliderJoint->setEntityB(nullptr);
        _sliderJoint->setLimits(-2.0f, 2.0f);
        _sliderJoint->setMotorSpeed(1.5f);
        _sliderJoint->setMaxMotorForce(400.0f);
    }

    void buildSpringCrate()
    {
        Entity* crate = createBox("crate", Vector3(0.7f, 0.7f, 0.7f),
            Vector3(5.0f, 4.0f, 0.0f), _wood.get(), RigidBodyType::Dynamic, 5.0f);
        if (auto* body = crate->findComponent<RigidBodyComponent>()) {
            body->setLinearDamping(0.2f);
        }

        // A small marker at the anchor, then the joint on the anchor entity: the
        // crate hangs below it on a sprung vertical axis.
        createPrimitive("box", _gray.get(), Vector3(5.0f, 4.0f, 0.0f),
            Vector3(0.2f, 0.2f, 0.2f));

        auto* joint = createJoint("anchor", Vector3(5.0f, 4.0f, 0.0f),
            Vector3(0.0f, 0.0f, 0.0f));
        joint->setType(PhysicsJointType::SixDof);
        joint->setEntityA(crate);
        joint->setEntityB(nullptr);
        joint->setLinearMotion(false, true, false);
        joint->setLinearSpring(Vector3(0.0f, 80.0f, 0.0f), Vector3(0.0f, 1.2f, 0.0f));
    }

    void buildBreakableTower()
    {
        std::vector<Entity*> boxes;
        for (int i = 0; i < 4; ++i) {
            boxes.push_back(createBox(("tower-" + std::to_string(i)).c_str(),
                Vector3(0.7f, 0.7f, 0.7f),
                Vector3(8.5f, 0.85f + static_cast<float>(i) * 0.7f, 0.0f),
                _green.get(), RigidBodyType::Dynamic, 8.0f));
        }

        for (int i = 0; i < 3; ++i) {
            Entity* lower = boxes[i];
            Entity* upper = boxes[i + 1];
            auto* weld = createJoint(("weld-" + std::to_string(i)).c_str(),
                Vector3(8.5f, 1.2f + static_cast<float>(i) * 0.7f, 0.0f),
                Vector3(0.0f, 0.0f, 0.0f));
            weld->setType(PhysicsJointType::Fixed);
            weld->setEntityA(upper);
            weld->setEntityB(lower);
            weld->setBreakImpulse(60.0f);

            // Tint both halves red when the weld parts.
            auto red = _red;
            weld->setOnBreak([lower, upper, red, i] {
                for (Entity* entity : {lower, upper}) {
                    if (auto* render = entity->findComponent<RenderComponent>()) {
                        for (auto* instance : render->meshInstances()) {
                            instance->setMaterial(red.get());
                        }
                    }
                }
                spdlog::info("weld-{} broke", i);
            });
        }
    }

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<StandardMaterial> _gray, _wood, _blue, _green, _red;
    struct PendingShot { RigidBodyComponent* body; Vector3 velocity; };
    std::vector<PendingShot> _pendingShots;
    Entity* _camera = nullptr;
    JointComponent* _sliderJoint = nullptr;
    Entity* _chainTail = nullptr;
    float _patrolTimer = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(PhysicsJointsExample)
