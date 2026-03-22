// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "plane.h"

namespace visutwin::canvas
{
    Plane::Plane() : _normal(0.0f, 1.0f, 0.0f), _distance(0.0f)
    {
    }

    Plane::Plane(const Vector3& normal, const float distance) : _normal(normal), _distance(distance)
    {
    }

    Plane& Plane::copy(const Plane& src)
    {
        _normal = src._normal;
        _distance = src._distance;
        return *this;
    }

    Plane Plane::clone() const
    {
        return Plane(_normal, _distance);
    }

    bool Plane::intersectsLine(const Vector3& start, const Vector3& end, Vector3* point) const
    {
        const float d0 = _normal.dot(start) + _distance;
        const float d1 = _normal.dot(end) + _distance;
        const float t = d0 / (d0 - d1);
        const bool intersects = t >= 0.0f && t <= 1.0f;
        if (intersects && point) {
            *point = start + (end - start) * t;
        }
        return intersects;
    }

    bool Plane::intersectsRay(const Ray& ray, Vector3* point) const
    {
        const float denominator = _normal.dot(ray.direction());
        if (denominator == 0.0f) {
            return false;
        }

        const float t = -(_normal.dot(ray.origin()) + _distance) / denominator;
        if (t >= 0.0f && point) {
            *point = ray.origin() + ray.direction() * t;
        }
        return t >= 0.0f;
    }

    Plane& Plane::normalize()
    {
        const float invLength = 1.0f / _normal.length();
        _normal = _normal * invLength;
        _distance *= invLength;
        return *this;
    }

    Plane& Plane::set(const float nx, const float ny, const float nz, const float d)
    {
        _normal = Vector3(nx, ny, nz);
        _distance = d;
        return *this;
    }

    Plane& Plane::setFromPointNormal(const Vector3& point, const Vector3& normal)
    {
        _normal = normal;
        _distance = -_normal.dot(point);
        return *this;
    }
}

