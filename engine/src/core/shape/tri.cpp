// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "tri.h"

#include <sstream>

namespace visutwin::canvas
{
    namespace
    {
        constexpr float kEpsilon = 1e-6f;
    }

    Tri::Tri() : _v0(0.0f), _v1(0.0f), _v2(0.0f)
    {
    }

    Tri::Tri(const Vector3& v0, const Vector3& v1, const Vector3& v2) : _v0(v0), _v1(v1), _v2(v2)
    {
    }

    Tri& Tri::set(const Vector3& v0, const Vector3& v1, const Vector3& v2)
    {
        _v0 = v0;
        _v1 = v1;
        _v2 = v2;
        return *this;
    }

    bool Tri::intersectsRay(const Ray& ray, Vector3* point) const
    {
        const Vector3 e1 = _v1 - _v0;
        const Vector3 e2 = _v2 - _v0;
        const Vector3 h = ray.direction().cross(e2);
        const float a = e1.dot(h);
        if (a > -kEpsilon && a < kEpsilon) {
            return false;
        }

        const float f = 1.0f / a;
        const Vector3 s = ray.origin() - _v0;
        const float u = f * s.dot(h);
        if (u < 0.0f || u > 1.0f) {
            return false;
        }

        const Vector3 q = s.cross(e1);
        const float v = f * ray.direction().dot(q);
        if (v < 0.0f || u + v > 1.0f) {
            return false;
        }

        const float t = f * e2.dot(q);
        if (t > kEpsilon) {
            if (point) {
                *point = ray.origin() + ray.direction() * t;
            }
            return true;
        }
        return false;
    }

    std::string Tri::toString() const
    {
        std::ostringstream stream;
        stream << "[[" << _v0.getX() << ", " << _v0.getY() << ", " << _v0.getZ()
            << "], [" << _v1.getX() << ", " << _v1.getY() << ", " << _v1.getZ()
            << "], [" << _v2.getX() << ", " << _v2.getY() << ", " << _v2.getZ() << "]]";
        return stream.str();
    }
}

