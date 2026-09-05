// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/math/vector3.h"
#include "framework/components/component.h"
#include "framework/physics/physicsWorld.h"

namespace visutwin::canvas
{
    /**
     * A constraint between two rigid bodies (upstream JointComponent).
     *
     * The joint lives on its OWN entity, and that entity's world transform is the
     * joint FRAME: its local **X axis is the primary axis** — the hinge's rotation
     * axis, the slider's travel axis, the ball joint's twist axis. Position and
     * orient the entity, then add the component; that is upstream's convention and
     * it is why the component takes two entities rather than an anchor offset.
     *
     * `entityB` may be null, which pins that end to the world.
     *
     * The joint is created lazily on the first update, because the bodies it needs
     * are themselves created on their first update. Any setter marks it stale and
     * it is rebuilt.
     */
    class JointComponent : public Component
    {
    public:
        JointComponent(IComponentSystem* system, Entity* entity);
        ~JointComponent() override;

        void initializeComponentData() override {}

        static const std::vector<JointComponent*>& instances() { return _instances; }

        PhysicsJointType type() const { return _type; }
        void setType(PhysicsJointType type);
        /// "fixed", "ball", "hinge", "slider" or "6dof".
        void setType(const std::string& type);

        Entity* entityA() const { return _entityA; }
        void setEntityA(Entity* entity);
        /// Null pins this end to the world.
        Entity* entityB() const { return _entityB; }
        void setEntityB(Entity* entity);

        /// Hinge angle in DEGREES or slider offset in metres, matching upstream's
        /// authoring units; converted to radians for the backend.
        void setLimits(float minimum, float maximum);
        void clearLimits();
        [[nodiscard]] bool hasLimits() const { return _enableLimits; }

        /// Degrees per second for a hinge, metres per second for a slider. The
        /// motor only acts once `setMaxMotorForce` is above zero, as upstream.
        void setMotorSpeed(float value);
        [[nodiscard]] float motorSpeed() const { return _motorSpeed; }
        void setMaxMotorForce(float value);

        /// Ball joint cone, in DEGREES.
        void setSwingLimits(float degreesY, float degreesZ);
        void setTwistLimit(float degrees);

        /// Above zero, the joint breaks when it carries more impulse than this.
        void setBreakImpulse(float value);
        [[nodiscard]] bool isBroken() const { return _broken; }
        /// Called once, on the update the joint breaks.
        void setOnBreak(std::function<void()> callback) { _onBreak = std::move(callback); }

        /// Six-degree-of-freedom only: which linear axes are free, and an optional
        /// spring per axis pulling toward `equilibrium`.
        void setLinearMotion(bool freeX, bool freeY, bool freeZ);
        void setLinearSpring(const Vector3& stiffness, const Vector3& equilibrium);

        void setJointEnabled(bool enabled);
        [[nodiscard]] bool jointEnabled() const;

        /// Called by JointComponentSystem.
        void syncToSimulation(PhysicsWorld& world);
        void releaseJoint(PhysicsWorld& world);

    private:
        void markStale() { _stale = true; }

        inline static std::vector<JointComponent*> _instances;

        PhysicsJointType _type = PhysicsJointType::Ball;
        Entity* _entityA = nullptr;
        Entity* _entityB = nullptr;

        bool _enableLimits = false;
        float _minLimit = 0.0f;
        float _maxLimit = 0.0f;
        float _motorSpeed = 0.0f;
        float _maxMotorForce = 0.0f;
        float _swingLimitY = 0.0f;
        float _swingLimitZ = 0.0f;
        float _twistLimit = 0.0f;
        float _breakImpulse = 0.0f;
        bool _linearFree[3] = {false, false, false};
        Vector3 _linearStiffness = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 _linearEquilibrium = Vector3(0.0f, 0.0f, 0.0f);

        PhysicsJoint* _joint = nullptr;
        PhysicsWorld* _world = nullptr;
        bool _stale = false;
        bool _wantEnabled = true;
        bool _broken = false;
        bool _warnedNoBody = false;
        std::function<void()> _onBreak;
    };
}
