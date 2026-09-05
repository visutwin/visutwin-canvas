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
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
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
            JPH::PhysicsSystem _system;
            BroadPhaseLayerMap _broadPhaseLayers;
            ObjectVsBroadPhaseFilter _objectVsBroadPhase;
            ObjectPairFilter _objectPairs;
            std::unique_ptr<JPH::TempAllocatorImpl> _tempAllocator;
            std::unique_ptr<JPH::JobSystemThreadPool> _jobs;
            std::vector<std::unique_ptr<JoltBody>> _owned;
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
            const JPH::BodyID id = (*it)->id();
            _system.GetBodyInterface().RemoveBody(id);
            _system.GetBodyInterface().DestroyBody(id);
            _entities.erase(id.GetIndexAndSequenceNumber());
            _owned.erase(it);
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
