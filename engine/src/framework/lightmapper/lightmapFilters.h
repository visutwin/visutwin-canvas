// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The CPU half of upstream's lightmap post-processing (lightmap-filters.js) and of its
// ambient bake (bake-light-ambient.js): the uniform block the dilate, bilateral denoise
// and ambient-occlusion passes read, and the virtual-light distribution and intensity the
// ambient bake uses. Pure functions, so tests/lightmapFiltersTests.cpp can hold them to
// upstream's numbers without a GPU.
//
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "core/math/vector3.h"

namespace visutwin::canvas::lightmap_filters
{
    /// Upstream DENOISE_FILTER_SIZE: the bilateral kernel is 15 x 15 texels; the shader's
    /// loop bound has to match.
    inline constexpr int kDenoiseFilterSize = 15;

    /// The uniform block of every lightmap filter pass (lightmapFilterShaders.h). The
    /// kernel is 15 floats, stored as four vec4s because a GLSL std140 float array would
    /// put each element on its own 16 bytes.
    struct alignas(16) LightmapFilterUniforms
    {
        float pixelOffset[2] = {0.0f, 0.0f};   // 1 / texture size (upstream `pixelOffset`)
        float sigmas[2] = {0.0f, 0.0f};        // filterRange, filterSmoothness (`sigmas`)
        float bZnorm = 1.0f;                   // 1 / normpdf(0, smoothness)
        float occlusionContrast = 0.0f;        // ambientBakeOcclusionContrast
        float occlusionBrightness = 0.0f;      // ambientBakeOcclusionBrightness
        float pad0 = 0.0f;
        float kernel[16] = {};                 // kDenoiseFilterSize used, the 16th is padding
    };
    static_assert(sizeof(LightmapFilterUniforms) == 96);

    /// Upstream's `normpdf` (lightmap-filters.js and bilateralDeNoise.js), constants and all.
    inline float normpdf(const float x, const float sigma)
    {
        return 0.39894f * std::exp(-0.5f * x * x / (sigma * sigma)) / sigma;
    }

    /// Upstream LightmapFilters.prepare + prepareDenoise + evaluateDenoiseUniforms: the
    /// spatial kernel from `filterRange`, the range normaliser from `filterSmoothness`.
    /// Upstream's scene setters keep the range above 0.001; so does this.
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

    /// Upstream bakeLmEnd's ambient-occlusion curve, applied to the accumulated visibility
    /// before it multiplies the ambient light.
    inline float ambientOcclusionCurve(const float occlusion, const float contrast, const float brightness)
    {
        const float shaped = (occlusion - 0.5f) * std::max(contrast + 1.0f, 0.0f) + 0.5f + brightness;
        return std::clamp(shaped, 0.0f, 1.0f);
    }

    /// Upstream random.spherePointDeterministic with start 0: evenly spread points over the
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

    /// The LINEAR intensity of one of upstream's ambient-bake virtual lights.
    /// BakeLightAmbient.prepareVirtualLight sets the light's intensity to
    /// `(pow(2 pi spherePart, 2.2) / N) ^ (1 / 2.2)`, and upstream's Light then shades with
    /// `linear(color) * intensity` when the intensity is at least 1 and with
    /// `linear(color * intensity)` below it (Light._updateLinearColor). This engine
    /// multiplies the decoded colour by the intensity in both cases, so the virtual light
    /// is given the value upstream ends up shading with — white, so the colour drops out.
    inline float ambientVirtualLightIntensity(const int numVirtualLights, const float spherePart)
    {
        constexpr float kGamma = 2.2f;
        constexpr float kPi = 3.14159265358979f;
        const float fullIntensity = 2.0f * kPi * spherePart;
        const float linearIntensity = std::pow(fullIntensity, kGamma);
        const float intensity = std::pow(linearIntensity / static_cast<float>(std::max(numVirtualLights, 1)),
            1.0f / kGamma);
        return intensity >= 1.0f ? intensity : std::pow(intensity, kGamma);
    }
}
