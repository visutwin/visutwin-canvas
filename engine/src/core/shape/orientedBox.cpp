// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "orientedBox.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace visutwin::canvas
{
    OrientedBox::OrientedBox() : _halfExtents(0.5f), _aabb()
    {
        _aabb.setCenter(0.0f, 0.0f, 0.0f);
        _aabb.setHalfExtents(_halfExtents);
    }

    OrientedBox::OrientedBox(const Matrix4& worldTransform, const Vector3& halfExtents)
        : _halfExtents(halfExtents), _modelTransform(worldTransform.inverse()), _worldTransform(worldTransform), _aabb()
    {
        _aabb.setCenter(0.0f, 0.0f, 0.0f);
        _aabb.setHalfExtents(_halfExtents);
    }

    void OrientedBox::setWorldTransform(const Matrix4& value)
    {
        _worldTransform = value;
        _modelTransform = value.inverse();
    }

    void OrientedBox::setHalfExtents(const Vector3& halfExtents)
    {
        _halfExtents = halfExtents;
        _aabb.setHalfExtents(_halfExtents);
    }

    bool OrientedBox::intersectsLocalAabbRay(const Ray& localRay, Vector3* localPoint) const
    {
        const Vector3& dir = localRay.direction();
        const Vector3& origin = localRay.origin();
        const Vector3 tMinDist = -_halfExtents - origin;
        const Vector3 tMaxDist = _halfExtents - origin;

        // Per axis, because a zero direction component selects instead of dividing.
        float txMin = tMinDist.getX();
        float txMax = tMaxDist.getX();
        float tyMin = tMinDist.getY();
        float tyMax = tMaxDist.getY();
        float tzMin = tMinDist.getZ();
        float tzMax = tMaxDist.getZ();

        if (dir.getX() == 0.0f) {
            txMin = txMin < 0.0f ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
            txMax = txMax < 0.0f ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        } else {
            txMin /= dir.getX();
            txMax /= dir.getX();
        }
        if (dir.getY() == 0.0f) {
            tyMin = tyMin < 0.0f ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
            tyMax = tyMax < 0.0f ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        } else {
            tyMin /= dir.getY();
            tyMax /= dir.getY();
        }
        if (dir.getZ() == 0.0f) {
            tzMin = tzMin < 0.0f ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
            tzMax = tzMax < 0.0f ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        } else {
            tzMin /= dir.getZ();
            tzMax /= dir.getZ();
        }

        const Vector3 tMin(txMin, tyMin, tzMin);
        const Vector3 tMax(txMax, tyMax, tzMax);
        const Vector3 realMin = Vector3::min(tMin, tMax);
        const Vector3 realMax = Vector3::max(tMin, tMax);

        const float minMax = std::min(std::min(realMax.getX(), realMax.getY()), realMax.getZ());
        const float maxMin = std::max(std::max(realMin.getX(), realMin.getY()), realMin.getZ());
        const bool intersects = minMax >= maxMin && maxMin >= 0.0f;
        if (intersects && localPoint) {
            *localPoint = localRay.origin() + localRay.direction() * maxMin;
        }
        return intersects;
    }

    bool OrientedBox::fastIntersectsLocalAabbRay(const Ray& localRay) const
    {
        const Vector3& diff = localRay.origin();
        const Vector3& dir = localRay.direction();
        const Vector3 absDiff = diff.abs();
        const Vector3 prod = diff * dir;

        if (absDiff.getX() > _halfExtents.getX() && prod.getX() >= 0.0f) {
            return false;
        }
        if (absDiff.getY() > _halfExtents.getY() && prod.getY() >= 0.0f) {
            return false;
        }
        if (absDiff.getZ() > _halfExtents.getZ() && prod.getZ() >= 0.0f) {
            return false;
        }

        const Vector3 absDir = dir.abs();
        const Vector3 cross = dir.cross(diff).abs();

        if (cross.getX() > _halfExtents.getY() * absDir.getZ() + _halfExtents.getZ() * absDir.getY()) {
            return false;
        }
        if (cross.getY() > _halfExtents.getX() * absDir.getZ() + _halfExtents.getZ() * absDir.getX()) {
            return false;
        }
        if (cross.getZ() > _halfExtents.getX() * absDir.getY() + _halfExtents.getY() * absDir.getX()) {
            return false;
        }

        return true;
    }

    Vector3 OrientedBox::closestPointOnLocalAabb(const Vector3& localPoint) const
    {
        return Vector3::clamp(localPoint, -_halfExtents, _halfExtents);
    }

    bool OrientedBox::intersectsRay(const Ray& ray, Vector3* point) const
    {
        Ray localRay;
        localRay.set(_modelTransform.transformPoint(ray.origin()), ray.direction().transformNormal(_modelTransform));

        if (point) {
            Vector3 localPoint;
            const bool intersects = intersectsLocalAabbRay(localRay, &localPoint);
            if (intersects) {
                *point = _worldTransform.transformPoint(localPoint);
            }
            return intersects;
        }

        return fastIntersectsLocalAabbRay(localRay);
    }

    bool OrientedBox::containsPoint(const Vector3& point) const
    {
        const Vector3 localDistance = _modelTransform.transformPoint(point).abs();
        return localDistance.getX() <= _halfExtents.getX() &&
            localDistance.getY() <= _halfExtents.getY() &&
            localDistance.getZ() <= _halfExtents.getZ();
    }

    bool OrientedBox::intersectsBoundingSphere(const BoundingSphere& sphere) const
    {
        const Vector3 localCenter = _modelTransform.transformPoint(sphere.center());
        const Vector3 closest = closestPointOnLocalAabb(localCenter);
        const Vector3 delta = localCenter - closest;
        return delta.lengthSquared() <= sphere.radius() * sphere.radius();
    }
}

