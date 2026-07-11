// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.07.2026.
//
#pragma once

#include <array>
#include <cstdint>

#include "core/math/vector3.h"

namespace visutwin::canvas
{
    /**
     * @brief Order-2 spherical harmonics projection of environment radiance for ambient light probes.
     * @ingroup group_scene_renderer
     *
     * Produces the 9 premultiplied irradiance coefficients consumed by Scene::setAmbientSH and
     * evaluated in the lit shader as
     * `sh[0] + sh[1]*x + sh[2]*y + sh[3]*z + sh[4]*xz + sh[5]*zy + sh[6]*yx + sh[7]*(3z^2-1) + sh[8]*(x^2-y^2)`.
     * The Ramamoorthi/Hanrahan convolution constants and the 1/pi Lambert normalization are folded
     * into the coefficients, so a uniform environment of radiance A yields a flat ambient of A —
     * identical semantics to Scene::setAmbientLight.
     */
    namespace sh
    {
        /// Project a float RGB(A) equirectangular radiance map (same direction convention as the
        /// skybox/IBL paths: u = atan2(x,z)/2pi + 0.5, v = 0.5 - asin(y)/pi) onto premultiplied SH9.
        /// @param pixels row-major, top row = v0; @param channels 3 or 4 floats per pixel.
        std::array<Vector3, 9> projectEquirect(const float* pixels, int width, int height, int channels = 4);

        /// 8-bit sRGB variant — pixels are converted to linear before projection.
        std::array<Vector3, 9> projectEquirect(const uint8_t* pixels, int width, int height, int channels = 4);

        /// Evaluate premultiplied SH9 irradiance in a (normalized) direction — CPU mirror of the
        /// shader path, useful for tests and debugging.
        Vector3 evaluate(const std::array<Vector3, 9>& coefficients, const Vector3& direction);
    }
}
