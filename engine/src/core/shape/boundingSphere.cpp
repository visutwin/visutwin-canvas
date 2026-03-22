// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "boundingSphere.h"

#include <cmath>

namespace visutwin::canvas
{
    BoundingSphere::BoundingSphere() : _center(0.0f), _radius(0.5f)
    {
    }

    BoundingSphere::BoundingSphere(const Vector3& center, const float radius) : _center(center), _radius(radius)
    {
    }

    BoundingSphere& BoundingSphere::copy(const BoundingSphere& src)
    {
        _center = src._center;
        _radius = src._radius;
        return *this;
    }

    BoundingSphere BoundingSphere::clone() const
    {
        return BoundingSphere(_center, _radius);
    }

    bool BoundingSphere::containsPoint(const Vector3& point) const
    {
        const Vector3 delta = point - _center;
        return delta.lengthSquared() < _radius * _radius;
    }

    bool BoundingSphere::intersectsRay(const Ray& ray, Vector3* point) const
    {
        const Vector3 m = ray.origin() - _center;
        const Vector3 rayDirection = ray.direction().normalized();
        const float b = m.dot(rayDirection);
        const float c = m.dot(m) - _radius * _radius;

        if (c > 0.0f && b > 0.0f) {
            return false;
        }

        const float discr = b * b - c;
        if (discr < 0.0f) {
            return false;
        }

        const float t = std::abs(-b - std::sqrt(discr));
        if (point) {
            *point = ray.origin() + rayDirection * t;
        }
        return true;
    }

    bool BoundingSphere::intersectsBoundingSphere(const BoundingSphere& sphere) const
    {
        const Vector3 delta = sphere._center - _center;
        const float totalRadius = sphere._radius + _radius;
        return delta.lengthSquared() <= totalRadius * totalRadius;
    }
}

