// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// What a light shades with (platform/graphics/lightRadiance.h), and that the main light
// array (deriveLighting, both backends) and the clustered lights (WorldClusters) take it
// from that one function.
//
// The oracle is the rule the upstream engine keeps for backwards compatibility in
// Light._updateLinearColor: at an intensity of 1 or more the colour is decoded with
// pow(c, 2.2) and then multiplied by the intensity, below 1 it is multiplied first and
// decoded after. A light at 0.5 therefore shades at 0.5^2.2 of its decoded colour.
//
// It also holds the clustered-light accept bit deriveLighting derives from the draw's
// mesh-instance mask.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <vector>

#include "platform/graphics/lightRadiance.h"
#include "platform/graphics/lightingDerivation.h"
#include "scene/constants.h"
#include "scene/lighting/worldClusters.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    bool nearRelative(const double a, const double b, const double tolerance = 1e-5)
    {
        return std::abs(a - b) <= tolerance * std::max(1.0, std::abs(b));
    }

    const Color kColor(0.8f, 0.5f, 0.25f, 1.0f);

    // The upstream rule in double: the light's linear colour, channel by channel.
    double referenceLinear(const double channel, const double intensity)
    {
        return intensity >= 1.0 ? std::pow(channel, 2.2) * intensity : std::pow(channel * intensity, 2.2);
    }

    void testLightRadiance()
    {
        std::cout << "lightRadiance\n";
        const float channels[3] = {kColor.r, kColor.g, kColor.b};

        // At 2 the decoded colour and the intensity go up unchanged, so a light at 1 or
        // more renders bit for bit as a plain decode-then-scale.
        const LightRadiance bright = lightRadiance(kColor, 2.0f);
        check(bright.intensity == 2.0f, "at 2 the intensity goes up as it is");
        bool exact = true;
        for (int c = 0; c < 3; ++c) {
            exact = exact && bright.linearColor[c] == gammaToLinear(channels[c]);
        }
        check(exact, "and the colour is the plain decode, bit for bit");

        // At 0.5 the colour is scaled in gamma space, then decoded, and the intensity is 1.
        const LightRadiance dim = lightRadiance(kColor, 0.5f);
        check(dim.intensity == 1.0f, "at 0.5 the intensity goes up as 1");
        bool matches = true;
        for (int c = 0; c < 3; ++c) {
            matches = matches && nearRelative(dim.linearColor[c] * dim.intensity, referenceLinear(channels[c], 0.5));
        }
        check(matches, "and the colour is decode(colour x 0.5)");
        check(nearRelative(dim.linearColor[0], std::pow(0.5, 2.2) * std::pow(0.8, 2.2), 1e-4),
            "which is 0.5^2.2 of the decoded colour, not 0.5 of it");

        // The boundary: exactly 1 takes the decode-then-scale branch.
        const LightRadiance one = lightRadiance(kColor, 1.0f);
        check(one.intensity == 1.0f && one.linearColor[1] == gammaToLinear(kColor.g), "1 is decode-then-scale");

        const LightRadiance off = lightRadiance(kColor, 0.0f);
        check(off.intensity == 0.0f && off.linearColor[0] == 0.0f && off.linearColor[2] == 0.0f,
            "0 goes up black at 0, so the shaders' intensity <= 0 skip still holds");
        const LightRadiance negative = lightRadiance(kColor, -1.0f);
        check(negative.intensity == 0.0f && negative.linearColor[1] == 0.0f, "a negative intensity is black too");
    }

    void testLinearScaleInverse()
    {
        std::cout << "\nthe linear scale and its inverse\n";
        check(linearScaleForIntensity(2.0f) == 2.0f, "at 2 the scale is the intensity");
        check(nearRelative(linearScaleForIntensity(0.5f), std::pow(0.5, 2.2)), "at 0.5 it is 0.5^2.2");
        check(linearScaleForIntensity(0.0f) == 0.0f && linearScaleForIntensity(-3.0f) == 0.0f,
            "zero and below shade nothing");
        bool roundTrips = true;
        for (const float scale : {0.001f, 0.075f, 0.3f, 0.999f, 1.0f, 1.6f, 40.0f}) {
            const float authored = intensityForLinearScale(scale);
            roundTrips = roundTrips && nearRelative(linearScaleForIntensity(authored), scale, 1e-5);
            // And through lightRadiance itself: a white light's colour times intensity.
            const LightRadiance r = lightRadiance(Color(1.0f, 1.0f, 1.0f, 1.0f), authored);
            roundTrips = roundTrips && nearRelative(static_cast<double>(r.linearColor[0]) * r.intensity, scale, 1e-5);
        }
        check(roundTrips, "intensityForLinearScale is the inverse, through lightRadiance too");

        // The lightmapper's soft directional copies: N copies of a light at 1.6 must sum to it.
        constexpr int kCopies = 15;
        const float copy = intensityForLinearScale(linearScaleForIntensity(1.6f) / kCopies);
        check(copy < 1.0f, "a 1.6 light split 15 ways is below 1 per copy");
        check(nearRelative(linearScaleForIntensity(copy) * kCopies, 1.6, 1e-5), "and the copies sum to the light");
    }

    GpuLightData light(const float intensity)
    {
        GpuLightData data;
        data.type = GpuLightType::Directional;
        data.color = kColor;
        data.intensity = intensity;
        return data;
    }

    void testDeriveLighting()
    {
        std::cout << "\nderiveLighting\n";
        const std::vector<GpuLightData> lights = {light(0.5f), light(2.0f)};
        const DerivedLighting derived = deriveLighting(Color(0.0f, 0.0f, 0.0f, 1.0f), lights, 8,
            FogParams{}, ShadowParams{}, nullptr, nullptr);
        check(derived.lightCount == 2, "two lights");
        bool dim = true;
        bool bright = true;
        const float channels[3] = {kColor.r, kColor.g, kColor.b};
        for (int c = 0; c < 3; ++c) {
            dim = dim && nearRelative(derived.lights[0].linearColor[c] * derived.lights[0].intensity,
                referenceLinear(channels[c], 0.5));
            bright = bright && derived.lights[1].linearColor[c] == gammaToLinear(channels[c]);
        }
        check(dim && derived.lights[0].intensity == 1.0f, "a light at 0.5 packs decode(colour x 0.5) at intensity 1");
        check(bright && derived.lights[1].intensity == 2.0f, "a light at 2 packs the decoded colour at intensity 2");

        // The clustered-light accept bit, from the draw's mesh-instance mask.
        const auto accept = [](const uint32_t mask) {
            return deriveLighting(Color(0.0f, 0.0f, 0.0f, 1.0f), {}, 8, FogParams{}, ShadowParams{},
                nullptr, nullptr, mask).clusterLightAccept;
        };
        check(deriveLighting(Color(0.0f, 0.0f, 0.0f, 1.0f), {}, 8, FogParams{}, ShadowParams{}, nullptr, nullptr)
                  .clusterLightAccept == MASK_AFFECT_DYNAMIC, "by default a draw accepts dynamic lights");
        check(accept(MASK_AFFECT_DYNAMIC) == MASK_AFFECT_DYNAMIC, "a dynamic mesh accepts dynamic lights");
        check(accept(MASK_AFFECT_DYNAMIC | MASK_AFFECT_LIGHTMAPPED) == MASK_AFFECT_DYNAMIC,
            "a mesh that is both counts as dynamic");
        check(accept(MASK_AFFECT_LIGHTMAPPED) == MASK_AFFECT_LIGHTMAPPED, "a lightmapped mesh accepts lightmapped lights");
        check(accept(MASK_BAKE) == MASK_AFFECT_LIGHTMAPPED && accept(MASK_NONE) == MASK_AFFECT_LIGHTMAPPED,
            "and so does any mesh without the dynamic bit");
    }

    void testClusteredRadiance()
    {
        std::cout << "\nclustered lights\n";
        ClusterLightData dim;
        dim.position = Vector3(0.0f, 0.0f, 0.0f);
        dim.range = 5.0f;
        dim.color = kColor;
        dim.intensity = 0.5f;
        ClusterLightData bright = dim;
        bright.position = Vector3(10.0f, 0.0f, 0.0f);
        bright.intensity = 2.0f;

        WorldClusters clusters;
        clusters.update({dim, bright});
        if (!check(clusters.lightCount() == 2, "both lights are clustered")) {
            return;
        }
        const GpuClusteredLight* packed = clusters.lightData();
        const LightRadiance dimRadiance = lightRadiance(kColor, 0.5f);
        const LightRadiance brightRadiance = lightRadiance(kColor, 2.0f);
        bool same = true;
        for (int c = 0; c < 3; ++c) {
            same = same && packed[0].colorIntensity[c] == dimRadiance.linearColor[c] &&
                   packed[1].colorIntensity[c] == brightRadiance.linearColor[c];
        }
        check(same && packed[0].colorIntensity[3] == dimRadiance.intensity &&
                  packed[1].colorIntensity[3] == brightRadiance.intensity,
            "the grid packs exactly what lightRadiance gives, as the main array does");
    }
}

int main()
{
    std::cout << std::unitbuf;
    testLightRadiance();
    testLinearScaleInverse();
    testDeriveLighting();
    testClusteredRadiance();
    return finish("light radiance");
}
