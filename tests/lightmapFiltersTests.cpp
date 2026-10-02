// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The CPU half of the GPU lightmapper's post-processing and ambient bake
// (framework/lightmapper/lightmapFilters.h), held to upstream's numbers:
//
// - the bilateral denoise uniforms (lightmap-filters.js evaluateDenoiseUniforms): a
//   symmetric 15-tap kernel of normpdf(j, filterRange), bZnorm = 1 / normpdf(0,
//   filterSmoothness), and the block laid out as the GLSL std140 / MSL struct expects —
//   a float array in std140 strides 16 bytes per element, which is why the kernel is
//   four vec4s and why its offset is checked;
// - the ambient virtual lights (bake-light-ambient.js): the sphere distribution
//   and the LINEAR intensity its Light ends up shading with, in both of the branches
//   Light._updateLinearColor takes;
// - bakeLmEnd's occlusion curve, at the values the lights-baked-a-o example uses.
//
// The shaders themselves run in the lightmap-bake example on both backends.

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

#include "framework/lightmapper/lightmapFilters.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;
using namespace visutwin::canvas::lightmap_filters;

namespace
{
    bool nearRelative(const double a, const double b, const double tolerance = 1e-5)
    {
        return std::abs(a - b) <= tolerance * std::max(1.0, std::abs(b));
    }

    // normpdf, in double, as the independent reference.
    double referenceNormpdf(const double x, const double sigma)
    {
        return 0.39894 * std::exp(-0.5 * x * x / (sigma * sigma)) / sigma;
    }

    void testDenoiseUniforms()
    {
        LightmapFilterUniforms u;
        prepareDenoise(u, 10.0f, 0.2f);
        check(u.sigmas[0] == 10.0f && u.sigmas[1] == 0.2f, "sigmas carry range and smoothness");
        for (int j = 0; j <= 7; ++j) {
            check(u.kernel[7 + j] == u.kernel[7 - j], "kernel is symmetric at " + std::to_string(j));
            check(nearRelative(u.kernel[7 + j], referenceNormpdf(j, 10.0)),
                "kernel tap " + std::to_string(j) + " is normpdf(j, range)");
        }
        check(u.kernel[15] == 0.0f, "the 16th kernel float is padding");
        check(nearRelative(u.bZnorm, 1.0 / referenceNormpdf(0.0, 0.2)), "bZnorm = 1 / normpdf(0, smoothness)");

        // The lightmap-sources example's values: range 5, smoothness 0.1.
        prepareDenoise(u, 5.0f, 0.1f);
        check(nearRelative(u.kernel[0], referenceNormpdf(7.0, 5.0)), "outer tap at range 5");
        check(nearRelative(u.bZnorm, 1.0 / referenceNormpdf(0.0, 0.1)), "bZnorm at smoothness 0.1");

        // A zero range would divide by zero; prepareDenoise keeps it above 0.001.
        prepareDenoise(u, 0.0f, 0.0f);
        check(std::isfinite(u.kernel[7]) && std::isfinite(u.bZnorm), "degenerate sigmas stay finite");

        prepare(u, 256, 128);
        check(u.pixelOffset[0] == 1.0f / 256.0f && u.pixelOffset[1] == 1.0f / 128.0f,
            "pixelOffset is one texel");
    }

    void testUniformLayout()
    {
        // GLSL std140: vec2 0, vec2 8, float 16, 20, 24, 28, vec4 kernel[4] at 32.
        check(offsetof(LightmapFilterUniforms, pixelOffset) == 0, "pixelOffset at 0");
        check(offsetof(LightmapFilterUniforms, sigmas) == 8, "sigmas at 8");
        check(offsetof(LightmapFilterUniforms, bZnorm) == 16, "bZnorm at 16");
        check(offsetof(LightmapFilterUniforms, occlusionContrast) == 20, "contrast at 20");
        check(offsetof(LightmapFilterUniforms, occlusionBrightness) == 24, "brightness at 24");
        check(offsetof(LightmapFilterUniforms, kernel) == 32, "kernel at 32 (vec4-aligned)");
        check(sizeof(LightmapFilterUniforms) == 96, "block is 96 bytes");
    }

    // intensity = (pow(2 pi part, 2.2) / N) ^ (1 / 2.2), shaded as intensity when
    // it is >= 1 and as intensity ^ 2.2 below that.
    double referenceAmbientLinear(const int n, const double part)
    {
        const double full = 2.0 * 3.14159265358979 * part;
        const double intensity = std::pow(std::pow(full, 2.2) / n, 1.0 / 2.2);
        return intensity >= 1.0 ? intensity : std::pow(intensity, 2.2);
    }

    void testAmbientVirtualLights()
    {
        // 20 samples over 0.4 of the sphere (the example's HUD): the < 1 branch, where the
        // N virtual lights sum to exactly pow(2 pi part, 2.2).
        const float linear20 = ambientVirtualLightIntensity(20, 0.4f);
        check(nearRelative(linear20, referenceAmbientLinear(20, 0.4), 1e-4), "20 samples, part 0.4");
        check(nearRelative(linear20 * 20.0, std::pow(2.0 * 3.14159265358979 * 0.4, 2.2), 1e-4),
            "below 1 the virtual lights sum to the full linear intensity");
        // One sample (the default ambientBakeNumSamples): the >= 1 branch.
        const float linear1 = ambientVirtualLightIntensity(1, 0.4f);
        check(nearRelative(linear1, referenceAmbientLinear(1, 0.4), 1e-4), "1 sample, part 0.4");
        check(nearRelative(linear1, 2.0 * 3.14159265358979 * 0.4, 1e-4), "one light shades with 2 pi part");
        check(nearRelative(ambientVirtualLightIntensity(64, 1.0f), referenceAmbientLinear(64, 1.0), 1e-4),
            "64 samples over the full sphere");

        // The distribution: unit vectors from the pole down to y = 1 - 2 part.
        for (const float part : {0.4f, 0.5f, 1.0f}) {
            const int n = 20;
            for (int i = 0; i < n; ++i) {
                const Vector3 p = spherePointDeterministic(i, n, part);
                check(nearRelative(p.length(), 1.0, 1e-5), "sphere point is a unit vector");
                check(p.getY() <= 1.0f && p.getY() >= 1.0f - 2.0f * part - 1e-5f,
                    "sphere point stays inside the sphere part");
            }
            const Vector3 first = spherePointDeterministic(0, n, part);
            check(nearRelative(first.getY(), 1.0), "the first point is the pole");
        }
    }

    void testOcclusionCurve()
    {
        // contrast 0, brightness 0: a plain saturate.
        check(ambientOcclusionCurve(0.25f, 0.0f, 0.0f) == 0.25f, "neutral curve passes 0.25");
        check(ambientOcclusionCurve(4.7f, 0.0f, 0.0f) == 1.0f, "neutral curve saturates");
        // lights-baked-a-o: contrast -0.6, brightness -0.5.
        check(nearRelative(ambientOcclusionCurve(1.0f, -0.6f, -0.5f), 0.2), "example curve at 1");
        check(ambientOcclusionCurve(0.5f, -0.6f, -0.5f) == 0.0f, "example curve at 0.5");
        check(ambientOcclusionCurve(5.0f, -0.6f, -0.5f) == 1.0f, "example curve saturates");
        // A contrast below -1 flattens to 0.5 + brightness rather than inverting.
        check(nearRelative(ambientOcclusionCurve(0.9f, -2.0f, 0.1f), 0.6), "contrast is clamped at -1");
    }
}

int main()
{
    quietPasses();
    testDenoiseUniforms();
    testUniformLayout();
    testAmbientVirtualLights();
    testOcclusionCurve();
    return finish("lightmap filters");
}
