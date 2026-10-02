// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#pragma once

#include <cmath>

namespace visutwin::canvas
{
    /// The lerp rate for a damping in [0, 1] over `dt` seconds (0 = no damping):
    /// 1 - damping^(dt * 1000).
    inline float damp(const float damping, const float dt)
    {
        return 1.0f - std::pow(damping, dt * 1000.0f);
    }
}
