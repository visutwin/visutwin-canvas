// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "rigidBodyComponentSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "core/shape/boundingSphere.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/physics/physicsWorld.h"

namespace visutwin::canvas
{
    namespace
    {
        /// Nearest first, with the entity pointer as a tiebreak so two hits at the same
        /// fraction keep a stable relative order across calls.
        void sortByHitFraction(std::vector<RaycastResult>& results)
        {
            std::sort(results.begin(), results.end(),
                [](const RaycastResult& a, const RaycastResult& b) {
                    if (std::abs(a.hitFraction - b.hitFraction) > 1e-6f) {
                        return a.hitFraction < b.hitFraction;
                    }
                    return a.entity < b.entity;
                });
        }
    }

    namespace
    {
        constexpr float EPS = 1e-6f;

        bool intersectSegmentSphere(
            const Vector3& start, const Vector3& end, const Vector3& center, const float radius,
            float& outT, Vector3& outPoint, Vector3& outNormal)
        {
            const Vector3 d = end - start;
            const float a = d.dot(d);
            if (a <= 1e-10f) {
                return false;
            }

            const Vector3 m = start - center;
            const float b = m.dot(d);
            const float c = m.dot(m) - radius * radius;
            if (c > 0.0f && b > 0.0f) {
                return false;
            }

            const float discr = b * b - a * c;
            if (discr < 0.0f) {
                return false;
            }

            const float sqrtDiscr = std::sqrt(discr);
            float t = (-b - sqrtDiscr) / a;
            if (t < 0.0f || t > 1.0f) {
                t = (-b + sqrtDiscr) / a;
                if (t < 0.0f || t > 1.0f) {
                    return false;
                }
            }

            outT = t;
            outPoint = start + d * t;
            outNormal = (outPoint - center).normalized();
            if (outNormal.lengthSquared() <= 1e-8f) {
                outNormal = Vector3(0.0f, 1.0f, 0.0f);
            }
            return true;
        }

        bool intersectSegmentAabb(
            const Vector3& start, const Vector3& end, const Vector3& halfExtents,
            float& outT, Vector3& outPoint, Vector3& outNormal)
        {
            const Vector3 d = end - start;
            float tMin = 0.0f;
            float tMax = 1.0f;
            Vector3 hitNormal(0.0f, 0.0f, 0.0f);

            const float minB[3] = {-halfExtents.getX(), -halfExtents.getY(), -halfExtents.getZ()};
            const float maxB[3] = { halfExtents.getX(),  halfExtents.getY(),  halfExtents.getZ()};
            const float s[3] = {start.getX(), start.getY(), start.getZ()};
            const float v[3] = {d.getX(), d.getY(), d.getZ()};

            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(v[axis]) < EPS) {
                    if (s[axis] < minB[axis] || s[axis] > maxB[axis]) {
                        return false;
                    }
                    continue;
                }

                float t1 = (minB[axis] - s[axis]) / v[axis];
                float t2 = (maxB[axis] - s[axis]) / v[axis];
                float enter = std::min(t1, t2);
                float exit = std::max(t1, t2);

                if (enter > tMin) {
                    tMin = enter;
                    if (axis == 0) hitNormal = Vector3((t1 > t2) ? 1.0f : -1.0f, 0.0f, 0.0f);
                    if (axis == 1) hitNormal = Vector3(0.0f, (t1 > t2) ? 1.0f : -1.0f, 0.0f);
                    if (axis == 2) hitNormal = Vector3(0.0f, 0.0f, (t1 > t2) ? 1.0f : -1.0f);
                }
                tMax = std::min(tMax, exit);
                if (tMin > tMax) {
                    return false;
                }
            }

            if (tMin < 0.0f || tMin > 1.0f) {
                return false;
            }

            outT = tMin;
            outPoint = start + d * tMin;
            outNormal = hitNormal.lengthSquared() > EPS ? hitNormal : Vector3(0.0f, 1.0f, 0.0f);
            return true;
        }

        bool intersectSegmentCapsuleY(
            const Vector3& start, const Vector3& end, const float radius, const float height,
            float& outT, Vector3& outPoint, Vector3& outNormal)
        {
            const Vector3 d = end - start;
            const float halfLine = std::max(0.0f, height * 0.5f - radius);
            bool found = false;
            float bestT = std::numeric_limits<float>::max();
            Vector3 bestPoint;
            Vector3 bestNormal;

            // Cylinder body (around Y axis).
            const float a = d.getX() * d.getX() + d.getZ() * d.getZ();
            const float b = 2.0f * (start.getX() * d.getX() + start.getZ() * d.getZ());
            const float c = start.getX() * start.getX() + start.getZ() * start.getZ() - radius * radius;
            if (a > EPS) {
                const float disc = b * b - 4.0f * a * c;
                if (disc >= 0.0f) {
                    const float sqrtDisc = std::sqrt(disc);
                    const float tCand[2] = {
                        (-b - sqrtDisc) / (2.0f * a),
                        (-b + sqrtDisc) / (2.0f * a)
                    };
                    for (const float t : tCand) {
                        if (t < 0.0f || t > 1.0f) {
                            continue;
                        }
                        const float y = start.getY() + d.getY() * t;
                        if (y < -halfLine || y > halfLine) {
                            continue;
                        }
                        if (t < bestT) {
                            const Vector3 p = start + d * t;
                            const Vector3 n = Vector3(p.getX(), 0.0f, p.getZ()).normalized();
                            bestT = t;
                            bestPoint = p;
                            bestNormal = n.lengthSquared() > EPS ? n : Vector3(1.0f, 0.0f, 0.0f);
                            found = true;
                        }
                    }
                }
            }

            // Hemispherical caps.
            const Vector3 capTop(0.0f, halfLine, 0.0f);
            const Vector3 capBottom(0.0f, -halfLine, 0.0f);
            float tSphere = 0.0f;
            Vector3 pSphere;
            Vector3 nSphere;
            if (intersectSegmentSphere(start, end, capTop, radius, tSphere, pSphere, nSphere) && tSphere < bestT) {
                bestT = tSphere;
                bestPoint = pSphere;
                bestNormal = nSphere;
                found = true;
            }
            if (intersectSegmentSphere(start, end, capBottom, radius, tSphere, pSphere, nSphere) && tSphere < bestT) {
                bestT = tSphere;
                bestPoint = pSphere;
                bestNormal = nSphere;
                found = true;
            }

            if (!found) {
                return false;
            }

            outT = bestT;
            outPoint = bestPoint;
            outNormal = bestNormal;
            return true;
        }

        // DEVIATION: until full Ammo/Bullet integration lands, raycast uses analytic primitive
        // intersections against current CollisionComponent shapes transformed by entity world matrix.
        bool intersectCollisionShape(
            const CollisionComponent* collision, const Vector3& start, const Vector3& end,
            float& outT, Vector3& outPoint, Vector3& outNormal)
        {
            if (!collision || !collision->entity()) {
                return false;
            }

            const Matrix4 world = collision->entity()->worldTransform();
            const Matrix4 invWorld = world.inverse();
            const Vector3 localStart = invWorld.transformPoint(start);
            const Vector3 localEnd = invWorld.transformPoint(end);

            float localT = 0.0f;
            Vector3 localPoint;
            Vector3 localNormal;

            const std::string type = collision->type();
            bool hit = false;
            if (type == "box") {
                hit = intersectSegmentAabb(localStart, localEnd, collision->halfExtents(), localT, localPoint, localNormal);
            } else if (type == "sphere") {
                hit = intersectSegmentSphere(localStart, localEnd, Vector3(0.0f, 0.0f, 0.0f), collision->radius(),
                    localT, localPoint, localNormal);
            } else if (type == "capsule") {
                hit = intersectSegmentCapsuleY(localStart, localEnd, collision->radius(), collision->height(),
                    localT, localPoint, localNormal);
            } else {
                // Fallback for unsupported shapes: conservative world-bounds sphere.
                float sphereT = 0.0f;
                Vector3 spherePoint;
                Vector3 sphereNormal;
                const BoundingSphere sphere = collision->worldBounds();
                hit = intersectSegmentSphere(start, end, sphere.center(), sphere.radius(), sphereT, spherePoint, sphereNormal);
                if (hit) {
                    outT = sphereT;
                    outPoint = spherePoint;
                    outNormal = sphereNormal;
                    return true;
                }
            }

            if (!hit) {
                return false;
            }

            const Vector3 worldPoint = world.transformPoint(localPoint);
            Vector3 worldNormal = localNormal.transformNormal(world).normalized();
            if (worldNormal.lengthSquared() <= EPS) {
                worldNormal = Vector3(0.0f, 1.0f, 0.0f);
            }

            outT = std::clamp(localT, 0.0f, 1.0f);
            outPoint = worldPoint;
            outNormal = worldNormal;
            return true;
        }
    }

    std::optional<RaycastResult> RigidBodyComponentSystem::raycastFirstCpu(const Vector3& start, const Vector3& end) const
    {
        const auto all = raycastAll(start, end);
        if (all.empty()) {
            return std::nullopt;
        }
        return all.front();
    }

    std::vector<RaycastResult> RigidBodyComponentSystem::raycastAllCpu(const Vector3& start, const Vector3& end) const
    {
        std::vector<RaycastResult> results;
        results.reserve(RigidBodyComponent::instances().size());

        for (auto* rigidbody : RigidBodyComponent::instances()) {
            if (!rigidbody || !rigidbody->enabled() || !rigidbody->entity()) {
                continue;
            }

            auto* collision = rigidbody->collision();
            if (!collision || !collision->enabled()) {
                continue;
            }

            float t = 0.0f;
            Vector3 point;
            Vector3 normal;
            if (!intersectCollisionShape(collision, start, end, t, point, normal)) {
                continue;
            }

            RaycastResult result;
            result.entity = rigidbody->entity();
            result.collision = collision;
            result.rigidbody = rigidbody;
            result.point = point;
            result.normal = normal;
            result.hitFraction = t;
            results.push_back(result);
        }

        sortByHitFraction(results);
        return results;
    }

    PhysicsWorld* RigidBodyComponentSystem::resolveWorld()
    {
        if (_world == nullptr) {
            _world = _engine ? _engine->physicsWorld() : nullptr;
        }
        return _world;
    }

    void RigidBodyComponentSystem::step(const float dt)
    {
        if (resolveWorld() == nullptr) {
            return;
        }

        const auto physicsStart = std::chrono::high_resolution_clock::now();

        // Create bodies BEFORE stepping so an entity added this frame is part of the
        // very first step rather than a frame behind. The same call writes kinematic
        // transforms into the simulation and reads dynamic ones back out, which is why
        // it runs on both sides of the step.
        for (auto* body : RigidBodyComponent::instances()) {
            if (body && body->enabled() && body->entity() && body->entity()->enabled()) {
                body->syncFromSimulation(*_world);
            }
        }
        _world->step(dt);
        for (auto* body : RigidBodyComponent::instances()) {
            if (body && body->enabled() && body->entity() && body->entity()->enabled()) {
                body->syncFromSimulation(*_world);
            }
        }

        // Accumulated across the substeps of one frame; Engine::update zeroes it.
        if (_engine && _engine->stats()) {
            const auto physicsEnd = std::chrono::high_resolution_clock::now();
            _engine->stats()->frame().physicsTime +=
                std::chrono::duration<double, std::milli>(physicsEnd - physicsStart).count();
        }
    }

    RigidBodyComponentSystem::RigidBodyComponentSystem(Engine* engine)
        : ComponentSystem(engine, "rigidbody")
    {
        if (engine == nullptr || engine->systems() == nullptr) {
            return;
        }

        // "fixedUpdate", not "update": Engine::update owns a fixed-timestep accumulator
        // and fires this zero or more times per frame with a constant delta. Stepping on
        // the variable frame delta made the simulation frame-rate dependent, and the
        // accumulator that exists to prevent exactly that had no subscribers at all.
        engine->systems()->on("fixedUpdate", [this](const float fixedDt) {
            if (resolveWorld() == nullptr) {
                return;
            }
            if (!(_timeScale > 0.0f)) {
                return;    // paused: nothing simulated, nothing written back
            }
            step(fixedDt * _timeScale);
        }, this);
    }

    RigidBodyComponentSystem::~RigidBodyComponentSystem()
    {
        if (_engine && _engine->systems()) {
            _engine->systems()->off("fixedUpdate", HandleEventCallback(), this);
        }
        if (_world != nullptr) {
            // Components can outlive the system, so hand their bodies back before
            // the world goes; a body freed twice is a crash on the way out.
            for (auto* body : RigidBodyComponent::instances()) {
                if (body) { body->releaseBody(*_world); }
            }
        }
    }

    std::optional<RaycastResult> RigidBodyComponentSystem::raycastFirst(
        const Vector3& start, const Vector3& end) const
    {
        if (_world == nullptr) {
            return raycastFirstCpu(start, end);
        }
        const auto hit = _world->raycastFirst(start, end);
        if (!hit) {
            return std::nullopt;
        }
        RaycastResult result;
        result.entity = hit->entity;
        result.point = hit->point;
        result.normal = hit->normal;
        result.hitFraction = hit->fraction;
        if (result.entity) {
            result.collision = result.entity->findComponent<CollisionComponent>();
            result.rigidbody = result.entity->findComponent<RigidBodyComponent>();
        }
        return result;
    }

    std::vector<RaycastResult> RigidBodyComponentSystem::raycastAll(
        const Vector3& start, const Vector3& end) const
    {
        if (_world == nullptr) {
            return raycastAllCpu(start, end);
        }
        std::vector<RaycastResult> results;
        for (const auto& hit : _world->raycastAll(start, end)) {
            RaycastResult result;
            result.entity = hit.entity;
            result.point = hit.point;
            result.normal = hit.normal;
            result.hitFraction = hit.fraction;
            if (result.entity) {
                result.collision = result.entity->findComponent<CollisionComponent>();
                result.rigidbody = result.entity->findComponent<RigidBodyComponent>();
            }
            results.push_back(result);
        }
        // Nearest first, the same order the CPU fallback returns. The physics world is
        // free to report hits in whatever order its broadphase walked them, so without
        // this the ordering silently depended on whether a world was supplied.
        sortByHitFraction(results);
        return results;
    }
}
