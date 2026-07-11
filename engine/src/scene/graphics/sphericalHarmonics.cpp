// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.07.2026.
//
#include "sphericalHarmonics.h"

#include <cmath>
#include <numbers>

namespace visutwin::canvas::sh
{
    namespace
    {
        constexpr float kPi = std::numbers::pi_v<float>;

        // Real SH basis constants (Y00, Y1x, Y2xy/Y2yz/Y2xz, Y20, Y22).
        constexpr float kY0 = 0.282095f;
        constexpr float kY1 = 0.488603f;
        constexpr float kY2xy = 1.092548f;
        constexpr float kY20 = 0.315392f;
        constexpr float kY22 = 0.546274f;

        // Ramamoorthi/Hanrahan irradiance convolution constants.
        constexpr float kC1 = 0.429043f;
        constexpr float kC2 = 0.511664f;
        constexpr float kC3 = 0.743125f;
        constexpr float kC4 = 0.886227f;

        // Accumulates raw radiance projections L_lm, then bakes convolution + 1/pi into the
        // premultiplied layout the shader evaluates directly.
        struct Projector
        {
            Vector3 l[9] = {};

            void accumulate(const Vector3& radiance, const Vector3& dir, float solidAngle)
            {
                const float x = dir.getX(), y = dir.getY(), z = dir.getZ();
                const float basis[9] = {
                    kY0,                    // L00
                    kY1 * x,                // L11
                    kY1 * y,                // L1-1
                    kY1 * z,                // L10
                    kY2xy * x * z,          // L21
                    kY2xy * z * y,          // L2-1
                    kY2xy * y * x,          // L2-2
                    kY20 * (3.0f * z * z - 1.0f), // L20
                    kY22 * (x * x - y * y)  // L22
                };
                for (int i = 0; i < 9; ++i) {
                    l[i] += radiance * (basis[i] * solidAngle);
                }
            }

            std::array<Vector3, 9> premultiplied() const
            {
                // E(n) = c4 L00 + 2c2 (L11 x + L1-1 y + L10 z)
                //      + 2c1 (L21 xz + L2-1 zy + L2-2 yx) + (c3/3) L20 (3z^2-1) + c1 L22 (x^2-y^2),
                // then / pi so uniform radiance A -> flat ambient A. Ordered to match the shader:
                // 1, x, y, z, xz, zy, yx, 3z^2-1, x^2-y^2.
                const float invPi = 1.0f / kPi;
                return {
                    l[0] * (kC4 * invPi),
                    l[1] * (2.0f * kC2 * invPi),
                    l[2] * (2.0f * kC2 * invPi),
                    l[3] * (2.0f * kC2 * invPi),
                    l[4] * (2.0f * kC1 * invPi),
                    l[5] * (2.0f * kC1 * invPi),
                    l[6] * (2.0f * kC1 * invPi),
                    l[7] * (kC3 / 3.0f * invPi),
                    l[8] * (kC1 * invPi)
                };
            }
        };

        template <typename FetchRgb>
        std::array<Vector3, 9> projectEquirectImpl(int width, int height, FetchRgb&& fetch)
        {
            Projector projector;
            if (width <= 0 || height <= 0) {
                return projector.premultiplied();
            }

            const float duv = (2.0f * kPi / static_cast<float>(width)) * (kPi / static_cast<float>(height));
            for (int py = 0; py < height; ++py) {
                // v flipped: top row is +Y pole (theta = +pi/2), matching toSphericalUv/dirToUvEquirect.
                const float v = (static_cast<float>(py) + 0.5f) / static_cast<float>(height);
                const float theta = (0.5f - v) * kPi;
                const float cosTheta = std::cos(theta);
                const float solidAngle = cosTheta * duv;
                const float y = std::sin(theta);
                for (int px = 0; px < width; ++px) {
                    const float u = (static_cast<float>(px) + 0.5f) / static_cast<float>(width);
                    const float phi = (u - 0.5f) * 2.0f * kPi;
                    const Vector3 dir(cosTheta * std::sin(phi), y, cosTheta * std::cos(phi));
                    projector.accumulate(fetch(px, py), dir, solidAngle);
                }
            }
            return projector.premultiplied();
        }
    }

    std::array<Vector3, 9> projectEquirect(const float* pixels, const int width, const int height, const int channels)
    {
        if (!pixels || channels < 3) {
            return {};
        }
        return projectEquirectImpl(width, height, [&](const int px, const int py) {
            const float* p = pixels + (static_cast<size_t>(py) * width + px) * channels;
            return Vector3(p[0], p[1], p[2]);
        });
    }

    std::array<Vector3, 9> projectEquirect(const uint8_t* pixels, const int width, const int height, const int channels)
    {
        if (!pixels || channels < 3) {
            return {};
        }
        const auto srgbToLinear = [](const uint8_t c) {
            const float f = static_cast<float>(c) / 255.0f;
            return f <= 0.04045f ? f / 12.92f : std::pow((f + 0.055f) / 1.055f, 2.4f);
        };
        return projectEquirectImpl(width, height, [&](const int px, const int py) {
            const uint8_t* p = pixels + (static_cast<size_t>(py) * width + px) * channels;
            return Vector3(srgbToLinear(p[0]), srgbToLinear(p[1]), srgbToLinear(p[2]));
        });
    }

    Vector3 evaluate(const std::array<Vector3, 9>& coefficients, const Vector3& direction)
    {
        const float x = direction.getX(), y = direction.getY(), z = direction.getZ();
        const Vector3 result = coefficients[0]
            + coefficients[1] * x + coefficients[2] * y + coefficients[3] * z
            + coefficients[4] * (x * z) + coefficients[5] * (z * y) + coefficients[6] * (y * x)
            + coefficients[7] * (3.0f * z * z - 1.0f) + coefficients[8] * (x * x - y * y);
        return Vector3(std::max(result.getX(), 0.0f), std::max(result.getY(), 0.0f), std::max(result.getZ(), 0.0f));
    }
}
