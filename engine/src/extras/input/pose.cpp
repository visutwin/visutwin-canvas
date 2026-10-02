// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.01.2026
//
#include "pose.h"

#include <algorithm>
#include <cmath>

#include "core/math/quaternion.h"

namespace visutwin::canvas
{
    namespace
    {
        // Angle lerp: the short way round, alpha clamped.
        float lerpAngle(const float a, float b, const float alpha)
        {
            if (b - a > 180.0f) {
                b -= 360.0f;
            }
            if (b - a < -180.0f) {
                b += 360.0f;
            }
            return a + (b - a) * std::clamp(alpha, 0.0f, 1.0f);
        }

        float clampRange(const float value, const Vector2& range)
        {
            return std::max(range.x, std::min(range.y, value));
        }
    }

    bool Pose::equalsApprox(const Pose& other, const float epsilon) const
    {
        const auto near3 = [epsilon](const Vector3& a, const Vector3& b) {
            return std::abs(a.getX() - b.getX()) < epsilon && std::abs(a.getY() - b.getY()) < epsilon &&
                std::abs(a.getZ() - b.getZ()) < epsilon;
        };
        return near3(position, other.position) && near3(angles, other.angles) &&
            std::abs(distance - other.distance) < epsilon;
    }

    Pose& Pose::lerp(const Pose& lhs, const Pose& rhs, const float alpha1, const float alpha2, const float alpha3)
    {
        position = lhs.position + (rhs.position - lhs.position) * alpha1;
        angles = Vector3(std::fmod(lerpAngle(lhs.angles.getX(), rhs.angles.getX(), alpha2), 360.0f),
                         std::fmod(lerpAngle(lhs.angles.getY(), rhs.angles.getY(), alpha2), 360.0f),
                         std::fmod(lerpAngle(lhs.angles.getZ(), rhs.angles.getZ(), alpha2), 360.0f));
        distance = lhs.distance + (rhs.distance - lhs.distance) * alpha3;
        return *this;
    }

    Pose& Pose::move(const Vector3& offset)
    {
        position += offset;
        position = Vector3(clampRange(position.getX(), xRange), clampRange(position.getY(), yRange),
                           clampRange(position.getZ(), zRange));
        return *this;
    }

    Pose& Pose::rotate(const Vector3& euler)
    {
        angles += euler;
        // Wrap into (-360, 360), as JavaScript's % does, then clamp pitch and yaw.
        const float x = std::fmod(angles.getX(), 360.0f);
        const float y = std::fmod(angles.getY(), 360.0f);
        const float z = std::fmod(angles.getZ(), 360.0f);
        angles = Vector3(clampRange(x, pitchRange), clampRange(y, yawRange), z);
        return *this;
    }

    Pose& Pose::set(const Vector3& newPosition, const Vector3& newAngles, const float newDistance)
    {
        position = newPosition;
        angles = newAngles;
        distance = newDistance;
        return *this;
    }

    Pose& Pose::look(const Vector3& from, const Vector3& to)
    {
        position = from;
        distance = from.distance(to);
        const Vector3 dir = (to - from).normalized();
        const float dx = dir.getX();
        const float dy = dir.getY();
        const float dz = dir.getZ();
        const float elev = std::atan2(-dy, std::sqrt(dx * dx + dz * dz)) * RAD_TO_DEG;
        const float azim = std::atan2(-dx, -dz) * RAD_TO_DEG;
        angles = Vector3(-elev, azim, 0.0f);
        return *this;
    }

    Vector3 Pose::getFocus() const
    {
        const Quaternion rotation = Quaternion::fromEulerAngles(angles.getX(), angles.getY(), angles.getZ());
        return rotation * Vector3(0.0f, 0.0f, -1.0f) * distance + position;
    }
}
