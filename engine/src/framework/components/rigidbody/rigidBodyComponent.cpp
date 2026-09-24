// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "rigidBodyComponent.h"

#include <spdlog/spdlog.h>

#include "framework/components/collision/collisionComponent.h"
#include "framework/components/joint/jointComponent.h"
#include "framework/entity.h"
#include "framework/physics/physicsWorld.h"

namespace visutwin::canvas
{
    namespace
    {
        PhysicsShapeType shapeFor(const CollisionComponent* collision)
        {
            if (collision == nullptr) {
                return PhysicsShapeType::Box;
            }
            const std::string& type = collision->type();
            if (type == "sphere") { return PhysicsShapeType::Sphere; }
            if (type == "capsule") { return PhysicsShapeType::Capsule; }
            if (type == "cylinder") { return PhysicsShapeType::Cylinder; }
            if (type == "plane") { return PhysicsShapeType::Plane; }
            return PhysicsShapeType::Box;
        }

        PhysicsMotionType motionFor(const RigidBodyType type)
        {
            switch (type) {
            case RigidBodyType::Dynamic:   return PhysicsMotionType::Dynamic;
            case RigidBodyType::Kinematic: return PhysicsMotionType::Kinematic;
            case RigidBodyType::Static:
            default:                       return PhysicsMotionType::Static;
            }
        }
    }

    RigidBodyComponent::RigidBodyComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    RigidBodyComponent::~RigidBodyComponent()
    {
        if (_world != nullptr && _body != nullptr) {
            JointComponent::bodyWillBeDestroyed(entity());
            _world->destroyBody(_body);
        }
        std::erase(_instances, this);
    }

    void RigidBodyComponent::setType(const RigidBodyType type)
    {
        if (_type != type) {
            _type = type;
            markBodyStale();
        }
    }

    void RigidBodyComponent::setType(const std::string& type)
    {
        if (type == "dynamic") {
            setType(RigidBodyType::Dynamic);
        } else if (type == "kinematic") {
            setType(RigidBodyType::Kinematic);
        } else {
            setType(RigidBodyType::Static);
        }
    }

    void RigidBodyComponent::setMass(const float value)
    {
        _mass = std::max(value, 0.0f);
        markBodyStale();
    }

    void RigidBodyComponent::setFriction(const float value)
    {
        _friction = std::clamp(value, 0.0f, 1.0f);
        markBodyStale();
    }

    void RigidBodyComponent::setRestitution(const float value)
    {
        _restitution = std::clamp(value, 0.0f, 1.0f);
        markBodyStale();
    }

    void RigidBodyComponent::setLinearDamping(const float value)
    {
        _linearDamping = std::max(value, 0.0f);
        markBodyStale();
    }

    void RigidBodyComponent::setAngularDamping(const float value)
    {
        _angularDamping = std::max(value, 0.0f);
        markBodyStale();
    }

    Vector3 RigidBodyComponent::linearVelocity() const
    {
        return _body ? _body->linearVelocity() : Vector3(0.0f, 0.0f, 0.0f);
    }

    void RigidBodyComponent::setLinearVelocity(const Vector3& value)
    {
        if (_body) { _body->setLinearVelocity(value); }
    }

    Vector3 RigidBodyComponent::angularVelocity() const
    {
        return _body ? _body->angularVelocity() : Vector3(0.0f, 0.0f, 0.0f);
    }

    void RigidBodyComponent::setAngularVelocity(const Vector3& value)
    {
        if (_body) { _body->setAngularVelocity(value); }
    }

    void RigidBodyComponent::applyForce(const Vector3& force)
    {
        if (_body) { _body->applyForce(force); }
    }

    void RigidBodyComponent::applyImpulse(const Vector3& impulse)
    {
        if (_body) { _body->applyImpulse(impulse); }
    }

    void RigidBodyComponent::applyTorque(const Vector3& torque)
    {
        if (_body) { _body->applyTorque(torque); }
    }

    void RigidBodyComponent::teleport(const Vector3& position)
    {
        if (entity()) {
            entity()->setPosition(position);
        }
        if (_body) {
            // setTransform deliberately does not wake the body, so a teleported
            // body would otherwise sit frozen in mid-air until something hit it.
            _body->setTransform(position, entity() ? entity()->rotation() : Quaternion());
            _body->setLinearVelocity(Vector3(0.0f, 0.0f, 0.0f));
            _body->setAngularVelocity(Vector3(0.0f, 0.0f, 0.0f));
            _body->activate();
        }
    }

    void RigidBodyComponent::activate()
    {
        if (_body) { _body->activate(); }
    }

    bool RigidBodyComponent::isActive() const
    {
        return _body != nullptr && _body->isActive();
    }

    CollisionComponent* RigidBodyComponent::collision() const
    {
        return entity() ? entity()->findComponent<CollisionComponent>() : nullptr;
    }

    void RigidBodyComponent::syncFromSimulation(PhysicsWorld& world)
    {
        Entity* owner = entity();
        if (owner == nullptr) {
            return;
        }

        if (_bodyStale && _body != nullptr) {
            // A joint on this body is freed with it; let it go first (see
            // JointComponent::bodyWillBeDestroyed).
            JointComponent::bodyWillBeDestroyed(owner);
            world.destroyBody(_body);
            _body = nullptr;
        }

        if (_body == nullptr) {
            const CollisionComponent* shape = collision();
            PhysicsBodyDesc desc;
            desc.shape = shapeFor(shape);
            desc.motion = motionFor(_type);
            if (shape != nullptr) {
                desc.halfExtents = shape->halfExtents();
                desc.radius = shape->radius();
                desc.height = shape->height();
            }
            desc.position = owner->position();
            desc.rotation = owner->rotation();
            desc.mass = _mass;
            desc.friction = _friction;
            desc.restitution = _restitution;
            desc.linearDamping = _linearDamping;
            desc.angularDamping = _angularDamping;
            desc.entity = owner;

            _body = world.createBody(desc);
            _world = &world;
            _bodyStale = false;
            if (_body == nullptr) {
                spdlog::warn("RigidBodyComponent: the physics world refused a body");
                return;
            }
        }

        if (_type == RigidBodyType::Static) {
            // A static body never moves, so writing its transform back every frame
            // would only fight whatever else owns that entity.
            return;
        }

        if (_type == RigidBodyType::Kinematic) {
            // The application drives a kinematic body; push the entity's transform
            // INTO the simulation rather than the other way round.
            _body->setTransform(owner->position(), owner->rotation());
            return;
        }

        const Vector3 scale = owner->localScale();
        if (scale.getX() < 0.0f || scale.getY() < 0.0f || scale.getZ() < 0.0f ||
            owner->worldScaleSign() < 0.0f) {
            setMirroredTransform(_body->position(), _body->rotation());
            return;
        }
        owner->setPosition(_body->position());
        owner->setRotation(_body->rotation());
    }

    // The rotation read from a mirrored world transform is NOT the entity's rotation:
    // Quaternion::fromMatrix4 negates the X axis of a mirrored basis to make it a
    // rotation, and a pair of negative scale factors reads as a 180-degree turn. The body
    // was created with that rotation, so writing it back as the WORLD rotation baked the
    // correction into the local rotation and the entity turned on its first step. Apply
    // instead the rotation the body turned through since the entity was last synced, to
    // the LOCAL rotation — upstream #9500. tests/mirroredBodyTests.cpp holds it.
    void RigidBodyComponent::setMirroredTransform(const Vector3& position, const Quaternion& bodyRotation)
    {
        Entity* owner = entity();
        // World-space rotation from the entity's current (read) rotation to the body's.
        Quaternion delta = bodyRotation * owner->rotation().invert();

        if (GraphNode* parent = owner->parent()) {
            // Expressed in the parent's space.
            const Quaternion parentRotation = parent->rotation();
            delta = parentRotation.invert() * delta * parentRotation;
            // A mirrored parent's rotation is read with its X axis negated, so the
            // parent's space is the mirror image of that rotation's: reflect the delta
            // through YZ.
            if (parent->worldScaleSign() < 0.0f) {
                delta = Quaternion(delta.getX(), -delta.getY(), -delta.getZ(), delta.getW());
            }
        }

        owner->setPosition(position);
        owner->setLocalRotation((delta * owner->localRotation()).normalized());
    }

    void RigidBodyComponent::releaseBody(PhysicsWorld& world)
    {
        if (_body != nullptr) {
            JointComponent::bodyWillBeDestroyed(entity());
            world.destroyBody(_body);
            _body = nullptr;
        }
        _world = nullptr;
        _bodyStale = false;
    }
}
