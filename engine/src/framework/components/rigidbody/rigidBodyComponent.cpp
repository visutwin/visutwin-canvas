// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
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
            if (type == "cone") { return PhysicsShapeType::Cone; }
            if (type == "mesh") { return PhysicsShapeType::Mesh; }
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
        _instanceList.add(this);
    }

    RigidBodyComponent::~RigidBodyComponent()
    {
        if (_world != nullptr && _body != nullptr) {
            JointComponent::bodyWillBeDestroyed(entity());
            _world->destroyBody(_body);
        }
        _instanceList.remove(this);
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
        const float mass = std::max(value, 0.0f);
        if (mass == _mass) {
            return;
        }
        _mass = mass;
        // Only a dynamic body reads its mass; any other type picks the new value up
        // when a type change rebuilds it.
        PhysicsBody* body = liveBody();
        if (body != nullptr && _type == RigidBodyType::Dynamic && !body->setMass(mass)) {
            markBodyStale();
        }
    }

    void RigidBodyComponent::setFriction(const float value)
    {
        const float friction = std::clamp(value, 0.0f, 1.0f);
        if (friction == _friction) {
            return;
        }
        _friction = friction;
        if (PhysicsBody* body = liveBody()) {
            body->setFriction(friction);
        }
    }

    void RigidBodyComponent::setRestitution(const float value)
    {
        const float restitution = std::clamp(value, 0.0f, 1.0f);
        if (restitution == _restitution) {
            return;
        }
        _restitution = restitution;
        if (PhysicsBody* body = liveBody()) {
            body->setRestitution(restitution);
        }
    }

    void RigidBodyComponent::setLinearDamping(const float value)
    {
        const float damping = std::max(value, 0.0f);
        if (damping == _linearDamping) {
            return;
        }
        _linearDamping = damping;
        if (PhysicsBody* body = liveBody()) {
            body->setDamping(_linearDamping, _angularDamping);
        }
    }

    void RigidBodyComponent::setAngularDamping(const float value)
    {
        const float damping = std::max(value, 0.0f);
        if (damping == _angularDamping) {
            return;
        }
        _angularDamping = damping;
        if (PhysicsBody* body = liveBody()) {
            body->setDamping(_linearDamping, _angularDamping);
        }
    }

    Vector3 RigidBodyComponent::linearVelocity() const
    {
        if (const PhysicsBody* body = liveBody()) {
            return body->linearVelocity();
        }
        if (_hasPendingLinearVelocity) {
            return _pendingLinearVelocity;
        }
        // A body awaiting its rebuild still moves as it did; the rebuild carries that over.
        return _body ? _body->linearVelocity() : Vector3(0.0f, 0.0f, 0.0f);
    }

    void RigidBodyComponent::setLinearVelocity(const Vector3& value)
    {
        if (PhysicsBody* body = liveBody()) {
            body->setLinearVelocity(value);
            return;
        }
        _pendingLinearVelocity = value;
        _hasPendingLinearVelocity = true;
        _pendingImpulse = Vector3(0.0f, 0.0f, 0.0f);
    }

    Vector3 RigidBodyComponent::angularVelocity() const
    {
        if (const PhysicsBody* body = liveBody()) {
            return body->angularVelocity();
        }
        if (_hasPendingAngularVelocity) {
            return _pendingAngularVelocity;
        }
        return _body ? _body->angularVelocity() : Vector3(0.0f, 0.0f, 0.0f);
    }

    void RigidBodyComponent::setAngularVelocity(const Vector3& value)
    {
        if (PhysicsBody* body = liveBody()) {
            body->setAngularVelocity(value);
            return;
        }
        _pendingAngularVelocity = value;
        _hasPendingAngularVelocity = true;
        _pendingTorqueImpulse = Vector3(0.0f, 0.0f, 0.0f);
    }

    void RigidBodyComponent::applyForce(const Vector3& force)
    {
        if (PhysicsBody* body = liveBody()) {
            body->applyForce(force);
            return;
        }
        _pendingForce += force;
        _hasPendingPushes = true;
    }

    void RigidBodyComponent::applyImpulse(const Vector3& impulse)
    {
        if (PhysicsBody* body = liveBody()) {
            body->applyImpulse(impulse);
            return;
        }
        _pendingImpulse += impulse;
        _hasPendingPushes = true;
    }

    void RigidBodyComponent::applyTorque(const Vector3& torque)
    {
        if (PhysicsBody* body = liveBody()) {
            body->applyTorque(torque);
            return;
        }
        _pendingTorque += torque;
        _hasPendingPushes = true;
    }

    void RigidBodyComponent::applyTorqueImpulse(const Vector3& impulse)
    {
        if (PhysicsBody* body = liveBody()) {
            body->applyTorqueImpulse(impulse);
            return;
        }
        _pendingTorqueImpulse += impulse;
        _hasPendingPushes = true;
    }

    void RigidBodyComponent::applyPendingTo(PhysicsBody& body)
    {
        // Velocities first: an impulse held after a velocity was set adds to it.
        if (_hasPendingLinearVelocity) {
            body.setLinearVelocity(_pendingLinearVelocity);
        }
        if (_hasPendingAngularVelocity) {
            body.setAngularVelocity(_pendingAngularVelocity);
        }
        if (_hasPendingPushes) {
            if (_pendingImpulse.lengthSquared() > 0.0f) {
                body.applyImpulse(_pendingImpulse);
            }
            if (_pendingTorqueImpulse.lengthSquared() > 0.0f) {
                body.applyTorqueImpulse(_pendingTorqueImpulse);
            }
            if (_pendingForce.lengthSquared() > 0.0f) {
                body.applyForce(_pendingForce);
            }
            if (_pendingTorque.lengthSquared() > 0.0f) {
                body.applyTorque(_pendingTorque);
            }
        }
        clearPending();
    }

    void RigidBodyComponent::clearPending()
    {
        const Vector3 zero(0.0f, 0.0f, 0.0f);
        _pendingLinearVelocity = zero;
        _pendingAngularVelocity = zero;
        _hasPendingLinearVelocity = false;
        _hasPendingAngularVelocity = false;
        _pendingImpulse = zero;
        _pendingTorqueImpulse = zero;
        _pendingForce = zero;
        _pendingTorque = zero;
        _hasPendingPushes = false;
    }

    void RigidBodyComponent::teleport(const Vector3& position)
    {
        if (entity()) {
            entity()->setPosition(position);
        }
        if (PhysicsBody* body = liveBody()) {
            // setTransform deliberately does not wake the body, so a teleported
            // body would otherwise sit frozen in mid-air until something hit it.
            body->setTransform(position, entity() ? entity()->rotation() : Quaternion());
            body->setLinearVelocity(Vector3(0.0f, 0.0f, 0.0f));
            body->setAngularVelocity(Vector3(0.0f, 0.0f, 0.0f));
            body->activate();
            return;
        }
        // The body yet to be created starts at the entity's new position; stopped means
        // no velocity and no impulse waiting to give it one.
        setLinearVelocity(Vector3(0.0f, 0.0f, 0.0f));
        setAngularVelocity(Vector3(0.0f, 0.0f, 0.0f));
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

    bool RigidBodyComponent::simulated() const
    {
        if (!active()) {
            return false;
        }
        const CollisionComponent* shape = collision();
        return shape == nullptr || shape->active();
    }

    void RigidBodyComponent::onDisable()
    {
        removeFromSimulation();
    }

    void RigidBodyComponent::removeFromSimulation()
    {
        if (_world != nullptr && _body != nullptr) {
            // A joint on this body is freed with it; let it go first.
            JointComponent::bodyWillBeDestroyed(entity());
            _world->destroyBody(_body);
        }
        _body = nullptr;
        _bodyStale = false;
        // Comes back at rest: neither the old body's motion nor anything asked of it
        // before it left carries over.
        clearPending();
    }

    void RigidBodyComponent::syncFromSimulation(PhysicsWorld& world)
    {
        Entity* owner = entity();
        if (owner == nullptr) {
            return;
        }

        if (!simulated()) {
            // Normally already done by onDisable; this also covers a state change that
            // reached no hook.
            if (_body != nullptr) {
                removeFromSimulation();
            }
            return;
        }

        if (_bodyStale && _body != nullptr) {
            // A rebuilt dynamic body keeps moving as the old one did, unless a velocity
            // was set since the rebuild was asked for.
            if (_type == RigidBodyType::Dynamic) {
                if (!_hasPendingLinearVelocity) {
                    _pendingLinearVelocity = _body->linearVelocity();
                    _hasPendingLinearVelocity = true;
                }
                if (!_hasPendingAngularVelocity) {
                    _pendingAngularVelocity = _body->angularVelocity();
                    _hasPendingAngularVelocity = true;
                }
            }
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
                if (desc.shape == PhysicsShapeType::Mesh) {
                    shape->collectMeshGeometry(desc.points, desc.indices);
                    // Jolt simulates a triangle mesh only on a static or kinematic body.
                    if (shape->convexHull() || desc.motion == PhysicsMotionType::Dynamic) {
                        desc.shape = PhysicsShapeType::ConvexHull;
                    }
                    if (desc.points.size() < 4 || (desc.shape == PhysicsShapeType::Mesh && desc.indices.empty())) {
                        spdlog::warn("RigidBodyComponent: mesh collision on '{}' has no usable geometry",
                            owner->name());
                    }
                }
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
            // What was asked of the body before it existed; this runs before the step,
            // so a held force acts on the body's first step.
            applyPendingTo(*_body);
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
    // the LOCAL rotation. tests/mirroredBodyTests.cpp holds it.
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
        clearPending();
    }

    void RigidBodyComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const RigidBodyComponent*>(source);
        if (!src) {
            return;
        }
        // Settings only. The clone's body is created from
        // them lazily, at the clone's own transform; velocities are simulation state.
        _type = src->_type;
        _mass = src->_mass;
        _friction = src->_friction;
        _restitution = src->_restitution;
        _linearDamping = src->_linearDamping;
        _angularDamping = src->_angularDamping;
        markBodyStale();
    }
}
