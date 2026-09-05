// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "joltPhysicsWorld.h"

#include <spdlog/spdlog.h>

#ifndef VISUTWIN_PHYSICS_JOLT

namespace visutwin::canvas
{
    std::shared_ptr<PhysicsWorld> createJoltPhysicsWorld()
    {
        spdlog::error(
            "createJoltPhysicsWorld: this build has no physics backend "
            "(configure with VISUTWIN_PHYSICS_JOLT=ON); nothing will simulate");
        return nullptr;
    }
}

#else

#include <algorithm>
#include <cstdarg>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

namespace visutwin::canvas
{
    namespace
    {
        // Two broad-phase layers is the standard split: everything static in one,
        // everything that can move in the other, so the broad phase never tests a
        // static pair against itself.
        namespace BroadPhaseLayers
        {
            constexpr JPH::BroadPhaseLayer NON_MOVING(0);
            constexpr JPH::BroadPhaseLayer MOVING(1);
            constexpr JPH::uint NUM_LAYERS = 2;
        }

        namespace ObjectLayers
        {
            constexpr JPH::ObjectLayer NON_MOVING = 0;
            constexpr JPH::ObjectLayer MOVING = 1;
            constexpr JPH::ObjectLayer NUM_LAYERS = 2;
        }

        class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface
        {
        public:
            BroadPhaseLayerMap()
            {
                _map[ObjectLayers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
                _map[ObjectLayers::MOVING] = BroadPhaseLayers::MOVING;
            }

            JPH::uint GetNumBroadPhaseLayers() const override
            {
                return BroadPhaseLayers::NUM_LAYERS;
            }

            JPH::BroadPhaseLayer GetBroadPhaseLayer(const JPH::ObjectLayer layer) const override
            {
                return _map[layer];
            }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
            {
                return layer == BroadPhaseLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
            }
#endif

        private:
            JPH::BroadPhaseLayer _map[ObjectLayers::NUM_LAYERS]{
                BroadPhaseLayers::NON_MOVING, BroadPhaseLayers::MOVING};
        };

        class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
        {
        public:
            bool ShouldCollide(const JPH::ObjectLayer layer1, const JPH::BroadPhaseLayer layer2) const override
            {
                // Static against static is the only pair worth skipping.
                return layer1 != ObjectLayers::NON_MOVING || layer2 == BroadPhaseLayers::MOVING;
            }
        };

        class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
        {
        public:
            bool ShouldCollide(const JPH::ObjectLayer a, const JPH::ObjectLayer b) const override
            {
                return a != ObjectLayers::NON_MOVING || b == ObjectLayers::MOVING;
            }
        };

        JPH::Vec3 toJolt(const Vector3& v) { return {v.getX(), v.getY(), v.getZ()}; }
        Vector3 fromJolt(const JPH::Vec3& v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
        JPH::Quat toJolt(const Quaternion& q) { return {q.getX(), q.getY(), q.getZ(), q.getW()}; }
        Quaternion fromJolt(const JPH::Quat& q)
        {
            return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
        }

#ifdef JPH_ENABLE_ASSERTS
        // Without this Jolt aborts on a failed assert with nothing on stderr, which
        // turns a mistake in a constraint description into a bare SIGTRAP.
        bool joltAssertFailed(const char* expression, const char* message,
            const char* file, const JPH::uint line)
        {
            spdlog::error("Jolt assert: {}:{}: ({}) {}", file, line, expression,
                message != nullptr ? message : "");
            return true;
        }
#endif

        void joltTrace(const char* format, ...)
        {
            char buffer[1024];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            spdlog::debug("Jolt: {}", buffer);
        }

        class JoltBody;
        class JoltJoint;

        class JoltWorld final : public PhysicsWorld
        {
        public:
            JoltWorld();
            ~JoltWorld() override;

            void step(float dt) override;
            void setGravity(const Vector3& gravity) override;
            [[nodiscard]] Vector3 gravity() const override;

            PhysicsBody* createBody(const PhysicsBodyDesc& desc) override;
            void destroyBody(PhysicsBody* body) override;

            PhysicsJoint* createJoint(const PhysicsJointDesc& desc) override;
            void destroyJoint(PhysicsJoint* joint) override;

            [[nodiscard]] std::optional<PhysicsRaycastHit> raycastFirst(
                const Vector3& start, const Vector3& end) const override;
            [[nodiscard]] std::vector<PhysicsRaycastHit> raycastAll(
                const Vector3& start, const Vector3& end) const override;

            JPH::BodyInterface& bodies() { return _system.GetBodyInterface(); }
            [[nodiscard]] const JPH::BodyInterface& bodies() const
            {
                return _system.GetBodyInterface();
            }
            [[nodiscard]] Entity* entityFor(JPH::BodyID id) const;

        private:
            /// Removes every joint that names `body` at either end. A constraint
            /// left pointing at a destroyed body is a dangling reference the
            /// solver will happily walk into.
            void destroyJointsTouching(const JoltBody* body);

            JPH::PhysicsSystem _system;
            BroadPhaseLayerMap _broadPhaseLayers;
            ObjectVsBroadPhaseFilter _objectVsBroadPhase;
            ObjectPairFilter _objectPairs;
            std::unique_ptr<JPH::TempAllocatorImpl> _tempAllocator;
            std::unique_ptr<JPH::JobSystemThreadPool> _jobs;
            std::vector<std::unique_ptr<JoltBody>> _owned;
            std::vector<std::unique_ptr<JoltJoint>> _joints;
            std::unordered_map<JPH::uint32, Entity*> _entities;
            // Jolt wants a fixed timestep; a long frame is split rather than
            // integrated in one jump, which is what makes a stack stay standing.
            float _accumulator = 0.0f;
        };

        class JoltBody final : public PhysicsBody
        {
        public:
            JoltBody(JoltWorld* world, const JPH::BodyID id) : _world(world), _id(id) {}

            [[nodiscard]] JPH::BodyID id() const { return _id; }

            Vector3 position() const override
            {
                return fromJolt(_world->bodies().GetPosition(_id));
            }
            Quaternion rotation() const override
            {
                return fromJolt(_world->bodies().GetRotation(_id));
            }
            void setTransform(const Vector3& position, const Quaternion& rotation) override
            {
                _world->bodies().SetPositionAndRotation(
                    _id, toJolt(position), toJolt(rotation), JPH::EActivation::DontActivate);
            }
            Vector3 linearVelocity() const override
            {
                return fromJolt(_world->bodies().GetLinearVelocity(_id));
            }
            void setLinearVelocity(const Vector3& value) override
            {
                _world->bodies().SetLinearVelocity(_id, toJolt(value));
            }
            Vector3 angularVelocity() const override
            {
                return fromJolt(_world->bodies().GetAngularVelocity(_id));
            }
            void setAngularVelocity(const Vector3& value) override
            {
                _world->bodies().SetAngularVelocity(_id, toJolt(value));
            }
            void applyForce(const Vector3& force) override
            {
                _world->bodies().AddForce(_id, toJolt(force));
            }
            void applyImpulse(const Vector3& impulse) override
            {
                _world->bodies().AddImpulse(_id, toJolt(impulse));
            }
            void applyTorque(const Vector3& torque) override
            {
                _world->bodies().AddTorque(_id, toJolt(torque));
            }
            void activate() override { _world->bodies().ActivateBody(_id); }
            [[nodiscard]] bool isActive() const override
            {
                return _world->bodies().IsActive(_id);
            }

        private:
            JoltWorld* _world;
            JPH::BodyID _id;
        };

        class JoltJoint final : public PhysicsJoint
        {
        public:
            JoltJoint(JPH::PhysicsSystem* system, JPH::Ref<JPH::Constraint> constraint,
                const JoltBody* bodyA, const JoltBody* bodyB, const float breakImpulse)
                : _system(system), _constraint(std::move(constraint)),
                  _bodyA(bodyA), _bodyB(bodyB), _breakImpulse(breakImpulse) {}

            ~JoltJoint() override
            {
                if (_system != nullptr && _constraint != nullptr) {
                    _system->RemoveConstraint(_constraint);
                }
            }

            void setEnabled(const bool enabled) override
            {
                if (_constraint != nullptr && !_broken) { _constraint->SetEnabled(enabled); }
            }

            [[nodiscard]] bool enabled() const override
            {
                return _constraint != nullptr && _constraint->GetEnabled();
            }

            void setMotorSpeed(const float speed) override
            {
                // Dispatched on the sub-type rather than through DynamicCast,
                // because Jolt's RTTI is an optional feature and this build does
                // not enable it.
                _motorSpeed = speed;
                if (_constraint == nullptr) {
                    return;
                }
                switch (_constraint->GetSubType()) {
                case JPH::EConstraintSubType::Hinge:
                    static_cast<JPH::HingeConstraint*>(_constraint.GetPtr())
                        ->SetTargetAngularVelocity(speed);
                    break;
                case JPH::EConstraintSubType::Slider:
                    static_cast<JPH::SliderConstraint*>(_constraint.GetPtr())
                        ->SetTargetVelocity(speed);
                    break;
                default:
                    break;
                }
            }

            [[nodiscard]] float motorSpeed() const override { return _motorSpeed; }
            [[nodiscard]] bool isBroken() const override { return _broken; }

            [[nodiscard]] bool touches(const JoltBody* body) const
            {
                return body != nullptr && (body == _bodyA || body == _bodyB);
            }

            /// Polled once per step. Jolt does not break constraints itself, so the
            /// world compares the impulse the solver applied last step against the
            /// threshold and disables the constraint for good.
            void updateBreak()
            {
                if (_broken || _breakImpulse <= 0.0f || _constraint == nullptr) {
                    return;
                }
                float carried = 0.0f;
                switch (_constraint->GetSubType()) {
                case JPH::EConstraintSubType::Fixed:
                    carried = static_cast<JPH::FixedConstraint*>(_constraint.GetPtr())
                        ->GetTotalLambdaPosition().Length();
                    break;
                case JPH::EConstraintSubType::Point:
                    carried = static_cast<JPH::PointConstraint*>(_constraint.GetPtr())
                        ->GetTotalLambdaPosition().Length();
                    break;
                case JPH::EConstraintSubType::Hinge:
                    carried = static_cast<JPH::HingeConstraint*>(_constraint.GetPtr())
                        ->GetTotalLambdaPosition().Length();
                    break;
                default:
                    return;
                }
                if (carried > _breakImpulse) {
                    _broken = true;
                    _constraint->SetEnabled(false);
                }
            }

        private:
            JPH::PhysicsSystem* _system;
            JPH::Ref<JPH::Constraint> _constraint;
            const JoltBody* _bodyA;
            const JoltBody* _bodyB;
            float _breakImpulse = 0.0f;
            float _motorSpeed = 0.0f;
            bool _broken = false;
        };

        JoltWorld::JoltWorld()
        {
            _tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
            _jobs = std::make_unique<JPH::JobSystemThreadPool>(
                JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1));

            _system.Init(
                /*maxBodies*/ 8192, /*numBodyMutexes*/ 0, /*maxBodyPairs*/ 8192,
                /*maxContactConstraints*/ 4096, _broadPhaseLayers, _objectVsBroadPhase,
                _objectPairs);
            _system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        }

        JoltWorld::~JoltWorld()
        {
            // Joints reference bodies, so they go first.
            _joints.clear();
            auto& bodyInterface = _system.GetBodyInterface();
            for (const auto& body : _owned) {
                bodyInterface.RemoveBody(body->id());
                bodyInterface.DestroyBody(body->id());
            }
            _owned.clear();
        }

        void JoltWorld::step(const float dt)
        {
            if (dt <= 0.0f) {
                return;
            }

            // Fixed 60 Hz substeps. Clamped to 8 so a stall (a breakpoint, a slow
            // first frame) cannot spiral into a hundred catch-up steps that stall
            // the next frame in turn.
            constexpr float fixedStep = 1.0f / 60.0f;
            constexpr int maxSteps = 8;

            _accumulator += std::min(dt, fixedStep * maxSteps);
            int steps = 0;
            while (_accumulator >= fixedStep && steps < maxSteps) {
                _system.Update(fixedStep, 1, _tempAllocator.get(), _jobs.get());
                for (const auto& joint : _joints) {
                    joint->updateBreak();
                }
                _accumulator -= fixedStep;
                ++steps;
            }
        }

        void JoltWorld::setGravity(const Vector3& gravity)
        {
            _system.SetGravity(toJolt(gravity));
        }

        Vector3 JoltWorld::gravity() const { return fromJolt(_system.GetGravity()); }

        PhysicsBody* JoltWorld::createBody(const PhysicsBodyDesc& desc)
        {
            JPH::ShapeSettings::ShapeResult shapeResult;
            switch (desc.shape) {
            case PhysicsShapeType::Sphere: {
                const JPH::SphereShapeSettings settings(std::max(desc.radius, 1e-3f));
                shapeResult = settings.Create();
                break;
            }
            case PhysicsShapeType::Capsule: {
                // CollisionComponent's height is the FULL height including the caps,
                // so the cylindrical half-height is (height - 2r) / 2.
                const float radius = std::max(desc.radius, 1e-3f);
                const float halfCylinder = std::max(desc.height * 0.5f - radius, 1e-3f);
                const JPH::CapsuleShapeSettings settings(halfCylinder, radius);
                shapeResult = settings.Create();
                break;
            }
            case PhysicsShapeType::Cylinder: {
                const JPH::CylinderShapeSettings settings(
                    std::max(desc.height * 0.5f, 1e-3f), std::max(desc.radius, 1e-3f));
                shapeResult = settings.Create();
                break;
            }
            case PhysicsShapeType::Plane: {
                // Jolt has no infinite plane primitive worth using here; a very wide,
                // thin box is the usual stand-in and behaves identically for contacts
                // that stay inside it.
                const JPH::BoxShapeSettings settings(JPH::Vec3(1000.0f, 0.5f, 1000.0f));
                shapeResult = settings.Create();
                break;
            }
            case PhysicsShapeType::Box:
            default: {
                const JPH::BoxShapeSettings settings(JPH::Vec3(
                    std::max(desc.halfExtents.getX(), 1e-3f),
                    std::max(desc.halfExtents.getY(), 1e-3f),
                    std::max(desc.halfExtents.getZ(), 1e-3f)));
                shapeResult = settings.Create();
                break;
            }
            }

            if (shapeResult.HasError()) {
                spdlog::error("JoltPhysicsWorld: shape creation failed ({})",
                    shapeResult.GetError().c_str());
                return nullptr;
            }

            const bool isStatic = desc.motion == PhysicsMotionType::Static;
            JPH::BodyCreationSettings settings(
                shapeResult.Get(), toJolt(desc.position), toJolt(desc.rotation),
                desc.motion == PhysicsMotionType::Dynamic ? JPH::EMotionType::Dynamic
                    : (isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Kinematic),
                isStatic ? ObjectLayers::NON_MOVING : ObjectLayers::MOVING);
            settings.mFriction = desc.friction;
            settings.mRestitution = desc.restitution;
            settings.mLinearDamping = desc.linearDamping;
            settings.mAngularDamping = desc.angularDamping;
            if (desc.motion == PhysicsMotionType::Dynamic && desc.mass > 0.0f) {
                settings.mOverrideMassProperties =
                    JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = desc.mass;
            }

            JPH::Body* body = _system.GetBodyInterface().CreateBody(settings);
            if (body == nullptr) {
                spdlog::error("JoltPhysicsWorld: body limit reached");
                return nullptr;
            }
            _system.GetBodyInterface().AddBody(
                body->GetID(),
                isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);

            _entities[body->GetID().GetIndexAndSequenceNumber()] = desc.entity;
            _owned.push_back(std::make_unique<JoltBody>(this, body->GetID()));
            return _owned.back().get();
        }

        void JoltWorld::destroyBody(PhysicsBody* body)
        {
            const auto it = std::find_if(_owned.begin(), _owned.end(),
                [body](const std::unique_ptr<JoltBody>& owned) { return owned.get() == body; });
            if (it == _owned.end()) {
                return;
            }
            destroyJointsTouching(it->get());
            const JPH::BodyID id = (*it)->id();
            _system.GetBodyInterface().RemoveBody(id);
            _system.GetBodyInterface().DestroyBody(id);
            _entities.erase(id.GetIndexAndSequenceNumber());
            _owned.erase(it);
        }

        PhysicsJoint* JoltWorld::createJoint(const PhysicsJointDesc& desc)
        {
            const auto* a = static_cast<const JoltBody*>(desc.bodyA);
            const auto* b = static_cast<const JoltBody*>(desc.bodyB);
            if (a == nullptr && b == nullptr) {
                spdlog::error("JoltPhysicsWorld: a joint needs a body at one end");
                return nullptr;
            }

            // Both ends have to be locked at once, and Jolt refuses two separate
            // same-priority locks — it asserts on the deadlock risk. BodyLockMulti
            // takes them together and orders them itself.
            JPH::BodyID ids[2];
            int idCount = 0;
            const int indexA = a != nullptr ? idCount++ : -1;
            if (a != nullptr) { ids[indexA] = a->id(); }
            const int indexB = b != nullptr ? idCount++ : -1;
            if (b != nullptr) { ids[indexB] = b->id(); }

            JPH::BodyLockMultiWrite lock(_system.GetBodyLockInterface(), ids, idCount);
            JPH::Body* joltA = a != nullptr ? lock.GetBody(indexA) : &JPH::Body::sFixedToWorld;
            JPH::Body* joltB = b != nullptr ? lock.GetBody(indexB) : &JPH::Body::sFixedToWorld;
            // Every constraint below is created as (B, A), not (A, B). Jolt
            // measures a constraint's angle and travel from body 1 toward body 2,
            // so putting the ANCHOR first is what makes a positive motor speed and
            // a positive limit move END A the way the caller means. Built the other
            // way round, a slider told to run at +1.5 travels at -1.5.
            if (joltA == nullptr || joltB == nullptr) {
                spdlog::error("JoltPhysicsWorld: a joint names a body that is gone");
                return nullptr;
            }

            // The frame's local X is the primary axis (upstream's convention); Y is
            // the reference direction the limits are measured from.
            const JPH::Quat frame = toJolt(desc.frameRotation).Normalized();
            const JPH::Vec3 point = toJolt(desc.framePosition);
            const JPH::Vec3 axisX = (frame * JPH::Vec3::sAxisX()).Normalized();
            const JPH::Vec3 axisY = (frame * JPH::Vec3::sAxisY()).Normalized();

            JPH::Ref<JPH::Constraint> constraint;
            switch (desc.type) {
            case PhysicsJointType::Fixed: {
                JPH::FixedConstraintSettings settings;
                settings.mSpace = JPH::EConstraintSpace::WorldSpace;
                settings.mAutoDetectPoint = false;
                settings.mPoint1 = settings.mPoint2 = point;
                settings.mAxisX1 = settings.mAxisX2 = axisX;
                settings.mAxisY1 = settings.mAxisY2 = axisY;
                constraint = settings.Create(*joltB, *joltA);
                break;
            }
            case PhysicsJointType::Hinge: {
                JPH::HingeConstraintSettings settings;
                settings.mSpace = JPH::EConstraintSpace::WorldSpace;
                settings.mPoint1 = settings.mPoint2 = point;
                settings.mHingeAxis1 = settings.mHingeAxis2 = axisX;
                settings.mNormalAxis1 = settings.mNormalAxis2 = axisY;
                if (desc.enableLimits) {
                    settings.mLimitsMin = desc.minLimit;
                    settings.mLimitsMax = desc.maxLimit;
                }
                if (desc.maxMotorForce > 0.0f) {
                    settings.mMotorSettings.mMaxForceLimit = desc.maxMotorForce;
                    settings.mMotorSettings.mMinForceLimit = -desc.maxMotorForce;
                }
                constraint = settings.Create(*joltB, *joltA);
                if (desc.maxMotorForce > 0.0f && constraint != nullptr) {
                    auto* hinge = static_cast<JPH::HingeConstraint*>(constraint.GetPtr());
                    hinge->SetMotorState(JPH::EMotorState::Velocity);
                    hinge->SetTargetAngularVelocity(desc.motorSpeed);
                }
                break;
            }
            case PhysicsJointType::Slider: {
                JPH::SliderConstraintSettings settings;
                settings.mSpace = JPH::EConstraintSpace::WorldSpace;
                settings.mAutoDetectPoint = false;
                settings.mPoint1 = settings.mPoint2 = point;
                settings.mSliderAxis1 = settings.mSliderAxis2 = axisX;
                settings.mNormalAxis1 = settings.mNormalAxis2 = axisY;
                if (desc.enableLimits) {
                    settings.mLimitsMin = desc.minLimit;
                    settings.mLimitsMax = desc.maxLimit;
                }
                if (desc.maxMotorForce > 0.0f) {
                    settings.mMotorSettings.mMaxForceLimit = desc.maxMotorForce;
                    settings.mMotorSettings.mMinForceLimit = -desc.maxMotorForce;
                }
                constraint = settings.Create(*joltB, *joltA);
                if (desc.maxMotorForce > 0.0f && constraint != nullptr) {
                    auto* slider = static_cast<JPH::SliderConstraint*>(constraint.GetPtr());
                    slider->SetMotorState(JPH::EMotorState::Velocity);
                    slider->SetTargetVelocity(desc.motorSpeed);
                }
                break;
            }
            case PhysicsJointType::SixDof: {
                JPH::SixDOFConstraintSettings settings;
                settings.mSpace = JPH::EConstraintSpace::WorldSpace;
                settings.mPosition1 = settings.mPosition2 = point;
                settings.mAxisX1 = settings.mAxisX2 = axisX;
                settings.mAxisY1 = settings.mAxisY2 = axisY;
                using Axis = JPH::SixDOFConstraintSettings::EAxis;
                const Axis linear[3] = {Axis::TranslationX, Axis::TranslationY, Axis::TranslationZ};
                const float stiffness[3] = {desc.linearStiffness.getX(),
                    desc.linearStiffness.getY(), desc.linearStiffness.getZ()};
                const float equilibrium[3] = {desc.linearEquilibrium.getX(),
                    desc.linearEquilibrium.getY(), desc.linearEquilibrium.getZ()};
                for (int i = 0; i < 3; ++i) {
                    if (!desc.linearFree[i]) {
                        settings.MakeFixedAxis(linear[i]);
                        continue;
                    }
                    settings.MakeFreeAxis(linear[i]);
                    if (stiffness[i] > 0.0f) {
                        // A sprung free axis: Jolt drives it to a target position
                        // with a stiffness/damping pair rather than a hard limit.
                        settings.mLimitsSpringSettings[static_cast<int>(linear[i])].mMode =
                            JPH::ESpringMode::StiffnessAndDamping;
                        settings.mLimitsSpringSettings[static_cast<int>(linear[i])].mStiffness =
                            stiffness[i];
                        settings.mLimitsSpringSettings[static_cast<int>(linear[i])].mDamping = 1.0f;
                        settings.mLimitMin[static_cast<int>(linear[i])] = equilibrium[i];
                        settings.mLimitMax[static_cast<int>(linear[i])] = equilibrium[i];
                    }
                }
                constraint = settings.Create(*joltB, *joltA);
                break;
            }
            case PhysicsJointType::Ball:
            default: {
                if (desc.swingLimitY > 0.0f || desc.swingLimitZ > 0.0f || desc.twistLimit > 0.0f) {
                    JPH::SwingTwistConstraintSettings settings;
                    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
                    settings.mPosition1 = settings.mPosition2 = point;
                    settings.mTwistAxis1 = settings.mTwistAxis2 = axisX;
                    settings.mPlaneAxis1 = settings.mPlaneAxis2 = axisY;
                    settings.mNormalHalfConeAngle = desc.swingLimitY;
                    settings.mPlaneHalfConeAngle = desc.swingLimitZ;
                    settings.mTwistMinAngle = -desc.twistLimit;
                    settings.mTwistMaxAngle = desc.twistLimit;
                    constraint = settings.Create(*joltB, *joltA);
                } else {
                    JPH::PointConstraintSettings settings;
                    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
                    settings.mPoint1 = settings.mPoint2 = point;
                    constraint = settings.Create(*joltB, *joltA);
                }
                break;
            }
            }

            if (constraint == nullptr) {
                spdlog::error("JoltPhysicsWorld: constraint creation failed");
                return nullptr;
            }

            _system.AddConstraint(constraint);
            _joints.push_back(
                std::make_unique<JoltJoint>(&_system, constraint, a, b, desc.breakImpulse));
            _joints.back()->setMotorSpeed(desc.motorSpeed);
            return _joints.back().get();
        }

        void JoltWorld::destroyJoint(PhysicsJoint* joint)
        {
            const auto it = std::find_if(_joints.begin(), _joints.end(),
                [joint](const std::unique_ptr<JoltJoint>& owned) { return owned.get() == joint; });
            if (it != _joints.end()) {
                _joints.erase(it);
            }
        }

        void JoltWorld::destroyJointsTouching(const JoltBody* body)
        {
            std::erase_if(_joints, [body](const std::unique_ptr<JoltJoint>& joint) {
                return joint->touches(body);
            });
        }

        Entity* JoltWorld::entityFor(const JPH::BodyID id) const
        {
            const auto it = _entities.find(id.GetIndexAndSequenceNumber());
            return it == _entities.end() ? nullptr : it->second;
        }

        std::optional<PhysicsRaycastHit> JoltWorld::raycastFirst(
            const Vector3& start, const Vector3& end) const
        {
            const JPH::Vec3 from = toJolt(start);
            const JPH::Vec3 direction = toJolt(end - start);
            const JPH::RRayCast ray{from, direction};

            JPH::RayCastResult result;
            if (!_system.GetNarrowPhaseQuery().CastRay(ray, result)) {
                return std::nullopt;
            }

            PhysicsRaycastHit hit;
            hit.fraction = result.mFraction;
            hit.point = start + (end - start) * result.mFraction;
            hit.entity = entityFor(result.mBodyID);

            JPH::BodyLockRead lock(_system.GetBodyLockInterface(), result.mBodyID);
            if (lock.Succeeded()) {
                hit.normal = fromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(
                    result.mSubShapeID2, toJolt(hit.point)));
            }
            return hit;
        }

        std::vector<PhysicsRaycastHit> JoltWorld::raycastAll(
            const Vector3& start, const Vector3& end) const
        {
            const JPH::RRayCast ray{toJolt(start), toJolt(end - start)};
            JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
            _system.GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings{}, collector);
            collector.Sort();

            std::vector<PhysicsRaycastHit> hits;
            hits.reserve(collector.mHits.size());
            for (const auto& raw : collector.mHits) {
                PhysicsRaycastHit hit;
                hit.fraction = raw.mFraction;
                hit.point = start + (end - start) * raw.mFraction;
                hit.entity = entityFor(raw.mBodyID);
                JPH::BodyLockRead lock(_system.GetBodyLockInterface(), raw.mBodyID);
                if (lock.Succeeded()) {
                    hit.normal = fromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(
                        raw.mSubShapeID2, toJolt(hit.point)));
                }
                hits.push_back(hit);
            }
            return hits;
        }

        // Jolt's globals are process-wide, so they are set up once however many
        // worlds an application makes.
        void ensureJoltInitialized()
        {
            static std::once_flag once;
            std::call_once(once, [] {
                JPH::RegisterDefaultAllocator();
                JPH::Trace = joltTrace;
#ifdef JPH_ENABLE_ASSERTS
                JPH::AssertFailed = joltAssertFailed;
#endif
                JPH::Factory::sInstance = new JPH::Factory();
                JPH::RegisterTypes();
            });
        }
    }

    std::shared_ptr<PhysicsWorld> createJoltPhysicsWorld()
    {
        ensureJoltInitialized();
        return std::make_shared<JoltWorld>();
    }
}

#endif // VISUTWIN_PHYSICS_JOLT
