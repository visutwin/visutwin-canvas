// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// What a light shades with: its authored colour and intensity turned into the linear
// colour and the scalar every light loop multiplies it by. One function for the main
// light array on both backends (through deriveLighting), the clustered lights
// (WorldClusters) and the local lights of the volumetric fog, so no two of them can
// treat a dimmed light differently.
//
#pragma once

#include <cmath>

#include "core/math/color.h"

namespace visutwin::canvas
{
    struct LightRadiance
    {
        float linearColor[3] = {0.0f, 0.0f, 0.0f};
        float intensity = 0.0f;
    };

    /// The light's linear colour, as `linearColor * intensity`.
    ///
    /// Deliberately reproduced quirk, kept so a scene authored for the engine this port
    /// derives from lights the same: at an intensity of 1 or more the colour is decoded to
    /// linear and then scaled, but BELOW 1 it is scaled in gamma space and then decoded,
    /// so a light dimmed under 1 falls off as intensity^2.2 rather than linearly (a white
    /// light at 0.5 shades at 0.22, not 0.5). Under physical units the intensity passed in
    /// is the luminance over the unit conversion, and the same rule applies to it.
    ///
    /// The split keeps a light at 1 or more bit-identical to a plain decode-then-scale:
    /// the decoded colour and the intensity go up unchanged. Below 1 the scaled decode goes
    /// up with an intensity of 1, and a light whose intensity is not positive goes up as
    /// black at 0, so the shaders' "intensity <= 0 contributes nothing" skip still holds.
    inline LightRadiance lightRadiance(const Color& color, const float intensity)
    {
        LightRadiance out;
        if (intensity >= 1.0f) {
            out.linearColor[0] = gammaToLinear(color.r);
            out.linearColor[1] = gammaToLinear(color.g);
            out.linearColor[2] = gammaToLinear(color.b);
            out.intensity = intensity;
        } else if (intensity > 0.0f) {
            out.linearColor[0] = gammaToLinear(color.r * intensity);
            out.linearColor[1] = gammaToLinear(color.g * intensity);
            out.linearColor[2] = gammaToLinear(color.b * intensity);
            out.intensity = 1.0f;
        }
        return out;
    }

    /// The factor by which lightRadiance scales a light's decoded colour at `intensity`:
    /// the intensity at 1 or more, its 2.2 power below (since (c i)^2.2 = c^2.2 i^2.2).
    inline float linearScaleForIntensity(const float intensity)
    {
        if (intensity >= 1.0f) {
            return intensity;
        }
        return intensity > 0.0f ? std::pow(intensity, 2.2f) : 0.0f;
    }

    /// The authored intensity at which lightRadiance scales a light's decoded colour by
    /// exactly `linearScale`: the inverse of linearScaleForIntensity. For code that has to
    /// split a light into copies whose contributions add up linearly (the lightmapper's
    /// soft directional copies).
    inline float intensityForLinearScale(const float linearScale)
    {
        if (linearScale >= 1.0f) {
            return linearScale;
        }
        return linearScale > 0.0f ? std::pow(linearScale, 1.0f / 2.2f) : 0.0f;
    }
}
