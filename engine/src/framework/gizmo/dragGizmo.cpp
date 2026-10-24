// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
#include "dragGizmo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace visutwin::canvas
{
    namespace
    {
        constexpr float LINE_EPSILON = 1e-8f;
    }

    DragGizmo::DragGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer, const std::string& name)
        : Gizmo(camera, std::move(layer), name)
    {
    }

    std::string DragGizmo::labelMarkup(const std::string& text, const Color& color)
    {
        const auto channel = [](const float c) {
            return static_cast<unsigned>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
        };
        char prefix[32];
        std::snprintf(prefix, sizeof(prefix), "[color=\"#%02x%02x%02x\"]", channel(color.r), channel(color.g),
            channel(color.b));
        return prefix + text + "[/color]";
    }

    float DragGizmo::snapValue(const float value, const float increment) const
    {
        return snap ? std::round(value / increment) * increment : value;
    }

    Vector3 DragGizmo::snapVector(const Vector3& value, const float increment) const
    {
        if (!snap) {
            return value;
        }
        return Vector3(std::round(value.getX() / increment) * increment, std::round(value.getY() / increment) * increment,
            std::round(value.getZ() / increment) * increment);
    }

    float DragGizmo::snapLength(const float value) const
    {
        if (!snap) {
            return value;
        }
        return std::max(std::round(value / snapIncrement), 1.0f) * snapIncrement;
    }

    float DragGizmo::snapAngle(const float value) const
    {
        return snapValue(value, snapAngleIncrement);
    }

    bool DragGizmo::lineNormal(const Vector3& dir, const Vector3& facing, Vector3& out)
    {
        const Vector3 side = dir.cross(facing);
        const bool valid = side.lengthSquared() >= LINE_EPSILON;
        out = side.normalized().cross(dir).normalized();
        return valid;
    }
}
