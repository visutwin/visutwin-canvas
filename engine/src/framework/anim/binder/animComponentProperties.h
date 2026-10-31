// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
#pragma once

#include <array>
#include <string>

namespace visutwin::canvas
{
    class Entity;

    /**
     * The component properties an animation curve may drive (AnimCurve::component /
     * propertyPath), read and written as up to four floats:
     *   light   color (r, g, b, a), intensity, range, innerConeAngle, outerConeAngle
     *   camera  fov
     * A property not in the table, or an entity without the component, reads and writes nothing.
     */
    bool readAnimComponentProperty(Entity* entity, const std::string& component, const std::string& property,
        std::array<float, 4>& value, int& components);

    bool writeAnimComponentProperty(Entity* entity, const std::string& component, const std::string& property,
        const std::array<float, 4>& value, int components);
}
