// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "jointComponent.h"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    }

    JointComponent::JointComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    JointComponent::~JointComponent()
    {
        if (_world != nullptr && _joint != nullptr) {
            _world->destroyJoint(_joint);
        }
        std::erase(_instances, this);
    }

    void JointComponent::setType(const PhysicsJointType type)
    {
        if (_type != type) {
            _type = type;
            markStale();
        }
    }

    void JointComponent::setType(const std::string& type)
    {
        if (type == "fixed") {
            setType(PhysicsJointType::Fixed);
        } else if (type == "hinge") {
            setType(PhysicsJointType::Hinge);
        } else if (type == "slider") {
            setType(PhysicsJointType::Slider);
        } else if (type == "6dof") {
            setType(PhysicsJointType::SixDof);
        } else {
            setType(PhysicsJointType::Ball);
        }
    }

    void JointComponent::setEntityA(Entity* entity)
    {
        if (_entityA != entity) {
            _entityA = entity;
            markStale();
        }
    }

    void JointComponent::setEntityB(Entity* entity)
    {
        if (_entityB != entity) {
            _entityB = entity;
            markStale();
        }
    }

    void JointComponent::setLimits(const float minimum, const float maximum)
    {
        _enableLimits = true;
        _minLimit = std::min(minimum, maximum);
        _maxLimit = std::max(minimum, maximum);
        markStale();
    }

    void JointComponent::clearLimits()
    {
        _enableLimits = false;
        markStale();
    }

    void JointComponent::setMotorSpeed(const float value)
    {
        _motorSpeed = value;
        // Live, so a scene can reverse a motor without rebuilding the constraint —
        // which is what upstream's patrolling slider does every few seconds.
        if (_joint != nullptr) {
            _joint->setMotorSpeed(_type == PhysicsJointType::Hinge ? value * kDegToRad : value);
        }
    }

    void JointComponent::setMaxMotorForce(const float value)
    {
        _maxMotorForce = std::max(value, 0.0f);
        markStale();
    }

    void JointComponent::setSwingLimits(const float degreesY, const float degreesZ)
    {
        _swingLimitY = std::max(degreesY, 0.0f);
        _swingLimitZ = std::max(degreesZ, 0.0f);
        markStale();
    }

    void JointComponent::setTwistLimit(const float degrees)
    {
        _twistLimit = std::max(degrees, 0.0f);
        markStale();
    }

    void JointComponent::setBreakImpulse(const float value)
    {
        _breakImpulse = std::max(value, 0.0f);
        markStale();
    }

    void JointComponent::setLinearMotion(const bool freeX, const bool freeY, const bool freeZ)
    {
        _linearFree[0] = freeX;
        _linearFree[1] = freeY;
        _linearFree[2] = freeZ;
        markStale();
    }

    void JointComponent::setLinearSpring(const Vector3& stiffness, const Vector3& equilibrium)
    {
        _linearStiffness = stiffness;
        _linearEquilibrium = equilibrium;
        markStale();
    }

    void JointComponent::setJointEnabled(const bool enabled)
    {
        _wantEnabled = enabled;
        if (_joint != nullptr) {
            _joint->setEnabled(enabled);
        }
    }

    bool JointComponent::jointEnabled() const
    {
        return _joint != nullptr ? _joint->enabled() : _wantEnabled;
    }

    void JointComponent::syncToSimulation(PhysicsWorld& world)
    {
        if (_joint != nullptr) {
            if (!_broken && _joint->isBroken()) {
                _broken = true;
                if (_onBreak) { _onBreak(); }
            }
            if (!_stale) {
                return;
            }
            world.destroyJoint(_joint);
            _joint = nullptr;
            _broken = false;
        }

        if (_entityA == nullptr) {
            if (!_warnedNoBody) {
                _warnedNoBody = true;
                spdlog::warn("JointComponent: entityA is not set; no joint will be created");
            }
            return;
        }

        auto* rigidA = _entityA->findComponent<RigidBodyComponent>();
        auto* rigidB = _entityB != nullptr
            ? _entityB->findComponent<RigidBodyComponent>() : nullptr;
        if (rigidA == nullptr || (_entityB != nullptr && rigidB == nullptr)) {
            if (!_warnedNoBody) {
                _warnedNoBody = true;
                spdlog::warn("JointComponent: an end has no RigidBodyComponent; "
                             "no joint will be created");
            }
            return;
        }

        PhysicsJointDesc desc;
        desc.bodyA = rigidA->physicsBody();
        desc.bodyB = rigidB != nullptr ? rigidB->physicsBody() : nullptr;
        if (desc.bodyA == nullptr || (_entityB != nullptr && desc.bodyB == nullptr)) {
            // The rigid bodies exist but have not reached their own first update
            // yet. Try again next frame rather than warning.
            return;
        }

        Entity* frame = entity();
        desc.type = _type;
        desc.framePosition = frame->position();
        desc.frameRotation = frame->rotation();
        desc.enableLimits = _enableLimits;
        // Hinge limits are authored in degrees, slider limits in metres.
        const bool angular = _type == PhysicsJointType::Hinge;
        desc.minLimit = angular ? _minLimit * kDegToRad : _minLimit;
        desc.maxLimit = angular ? _maxLimit * kDegToRad : _maxLimit;
        desc.motorSpeed = angular ? _motorSpeed * kDegToRad : _motorSpeed;
        desc.maxMotorForce = _maxMotorForce;
        desc.swingLimitY = _swingLimitY * kDegToRad;
        desc.swingLimitZ = _swingLimitZ * kDegToRad;
        desc.twistLimit = _twistLimit * kDegToRad;
        desc.breakImpulse = _breakImpulse;
        desc.linearFree[0] = _linearFree[0];
        desc.linearFree[1] = _linearFree[1];
        desc.linearFree[2] = _linearFree[2];
        desc.linearStiffness = _linearStiffness;
        desc.linearEquilibrium = _linearEquilibrium;

        _joint = world.createJoint(desc);
        _world = &world;
        _stale = false;
        if (_joint != nullptr) {
            _joint->setEnabled(_wantEnabled);
        }
    }

    void JointComponent::releaseJoint(PhysicsWorld& world)
    {
        if (_joint != nullptr) {
            world.destroyJoint(_joint);
            _joint = nullptr;
        }
        _world = nullptr;
        _stale = false;
    }
}
