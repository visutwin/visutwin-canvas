// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The CPU half of the lightmap post-processing and of the ambient bake: the uniform
// block the dilate, bilateral denoise and ambient-occlusion passes read, and the
// virtual-light distribution and intensity the ambient bake uses. Pure functions, so
// tests/lightmapFiltersTests.cpp can check them without a GPU.
//
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "core/math/vector3.h"

namespace visutwin::canvas::lightmap_filters
{
    /// The bilateral kernel is 15 x 15 texels; the shader's
    /// loop bound has to match.
    inline constexpr int kDenoiseFilterSize = 15;

    /// The uniform block of every lightmap filter pass (lightmapFilterShaders.h). The
    /// kernel is 15 floats, stored as four vec4s because a GLSL std140 float array would
    /// put each element on its own 16 bytes.
    struct alignas(16) LightmapFilterUniforms
    {
        float pixelOffset[2] = {0.0f, 0.0f};   // 1 / texture size
        float sigmas[2] = {0.0f, 0.0f};        // filterRange, filterSmoothness (`sigmas`)
        float bZnorm = 1.0f;                   // 1 / normpdf(0, smoothness)
        float occlusionContrast = 0.0f;        // ambientBakeOcclusionContrast
        float occlusionBrightness = 0.0f;      // ambientBakeOcclusionBrightness
        float pad0 = 0.0f;
        float kernel[16] = {};                 // kDenoiseFilterSize used, the 16th is padding
    };
    static_assert(sizeof(LightmapFilterUniforms) == 96);

    /// The normal probability density the denoise kernel weights by.
    inline float normpdf(const float x, const float sigma)
    {
        return 0.39894f * std::exp(-0.5f * x * x / (sigma * sigma)) / sigma;
    }

    /// The spatial kernel from `filterRange`, the range normaliser from `filterSmoothness`.
    /// The range is kept above 0.001.
    inline void prepareDenoise(LightmapFilterUniforms& u, const float filterRange,
        const float filterSmoothness)
    {
        const float range = std::max(filterRange, 0.001f);
        const float smoothness = std::max(filterSmoothness, 0.001f);
        u.sigmas[0] = range;
        u.sigmas[1] = smoothness;
        constexpr int kSize = (kDenoiseFilterSize - 1) / 2;
        for (int j = 0; j <= kSize; ++j) {
            const float value = normpdf(static_cast<float>(j), range);
            u.kernel[kSize + j] = value;
            u.kernel[kSize - j] = value;
        }
        u.bZnorm = 1.0f / normpdf(0.0f, smoothness);
    }

    inline void prepare(LightmapFilterUniforms& u, const int width, const int height)
    {
        u.pixelOffset[0] = 1.0f / static_cast<float>(std::max(width, 1));
        u.pixelOffset[1] = 1.0f / static_cast<float>(std::max(height, 1));
    }

    /// The ambient-occlusion curve, applied to the accumulated visibility
    /// before it multiplies the ambient light.
    inline float ambientOcclusionCurve(const float occlusion, const float contrast, const float brightness)
    {
        const float shaped = (occlusion - 0.5f) * std::max(contrast + 1.0f, 0.0f) + 0.5f + brightness;
        return std::clamp(shaped, 0.0f, 1.0f);
    }

    /// Evenly spread points over the
    /// top `end` part of the unit sphere (0.5 the upper hemisphere, 1 all of it).
    inline Vector3 spherePointDeterministic(const int index, const int numPoints, const float end)
    {
        constexpr float kGoldenAngle = 2.399963229728653f;
        const float finish = 1.0f - 2.0f * end;
        const float t = static_cast<float>(index) / static_cast<float>(std::max(numPoints, 1));
        const float y = 1.0f + (finish - 1.0f) * t;
        const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float theta = kGoldenAngle * static_cast<float>(index);
        return Vector3(std::cos(theta) * radius, y, std::sin(theta) * radius);
    }

    /// The AUTHORED intensity of one ambient-bake virtual light,
    /// `(pow(2 pi spherePart, 2.2) / N) ^ (1 / 2.2)`. The light is shaded through
    /// lightRadiance (platform/graphics/lightRadiance.h), which decodes then scales at 1
    /// or more and scales then decodes below it, so for a white light the value it shades
    /// with is this intensity at 1 or more and its 2.2 power below; with more than one
    /// light that makes the N virtual lights sum to exactly `pow(2 pi spherePart, 2.2)`.
    inline float ambientVirtualLightIntensity(const int numVirtualLights, const float spherePart)
    {
        constexpr float kGamma = 2.2f;
        constexpr float kPi = 3.14159265358979f;
        const float fullIntensity = 2.0f * kPi * spherePart;
        const float linearIntensity = std::pow(fullIntensity, kGamma);
        return std::pow(linearIntensity / static_cast<float>(std::max(numVirtualLights, 1)), 1.0f / kGamma);
    }
}
