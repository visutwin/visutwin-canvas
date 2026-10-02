// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Conversions from the Phong material model (OBJ/MTL, Assimp's legacy materials) to this
// engine's metal-roughness parameters, shared so every parser turns the same file into the
// same material.
//
#pragma once

#include <algorithm>
#include <cmath>

namespace visutwin::canvas
{
    /// Perceptual roughness for a Phong specular exponent. A Blinn-Phong lobe of exponent n
    /// matches a microfacet distribution of alpha = sqrt(2 / (n + 2)) (Walter et al. 2007),
    /// and the engine's BRDF takes alpha = roughness^2, so roughness = alpha^(1/2). n = 0 is
    /// fully rough; n is clamped to [0, 10000], beyond which the lobe is a mirror anyway.
    inline float roughnessFromShininess(float shininess)
    {
        const float n = std::clamp(shininess, 0.0f, 10000.0f);
        return std::pow(2.0f / (n + 2.0f), 0.25f);
    }
}
