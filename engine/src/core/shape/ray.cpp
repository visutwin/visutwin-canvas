// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "ray.h"

namespace visutwin::canvas
{
    Ray::Ray() : _origin(0.0f), _direction(0.0f, 0.0f, -1.0f)
    {
    }

    Ray::Ray(const Vector3& origin, const Vector3& direction) : _origin(origin), _direction(direction)
    {
    }

    Ray& Ray::set(const Vector3& origin, const Vector3& direction)
    {
        _origin = origin;
        _direction = direction;
        return *this;
    }

    Ray& Ray::copy(const Ray& src)
    {
        return set(src._origin, src._direction);
    }

    Ray Ray::clone() const
    {
        return Ray(_origin, _direction);
    }
}

