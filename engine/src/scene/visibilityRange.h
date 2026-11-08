// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
// The camera distances at which a mesh instance is drawn (MeshInstance::setVisibilityRange):
// hidden nearer than fadeInStart, fading in up to fadeInEnd, fully drawn up to fadeOutStart,
// fading out up to fadeOutEnd and hidden beyond. The distance is from the camera to the centre
// of the instance's world bounds. Levels of detail are instances whose ranges meet: the near
// level fades out over the band the far level fades in over ([10, 12] for both, say), and
// within it each draws the complementary half of a dither pattern, so the two cover every
// pixel once. A band of zero width is a hard switch at that distance, again with no gap and no
// overlap: the near level is drawn below it, the far one from it on.
//
#pragma once

#include <limits>

namespace visutwin::canvas
{
    struct VisibilityRange
    {
        static constexpr float kInfinity = std::numeric_limits<float>::infinity();

        float fadeInStart = 0.0f;
        float fadeInEnd = 0.0f;
        float fadeOutStart = kInfinity;
        float fadeOutEnd = kInfinity;

        /// Whether the range hides the instance at any distance (the default does not).
        bool limits() const { return fadeInEnd > 0.0f || fadeOutStart < kInfinity; }

        /**
         * What the range does to the instance at `distance`: 1 drawn whole, 0 hidden, and
         * inside a band the fraction f of its fragments that are drawn, as f while it fades
         * OUT and -f while it fades IN (the shader keeps complementary halves of the dither
         * for the two). Where the bands overlap the fade-out band wins. Each fraction is
         * computed from the band it is in alone, so a level fading out at d and the next fading
         * in over the same band at d give f and -(1 - f), to rounding.
         */
        float fadeAt(const float distance) const
        {
            if (distance >= fadeOutEnd) {
                return 0.0f;
            }
            if (distance >= fadeOutStart) {
                return (fadeOutEnd - distance) / (fadeOutEnd - fadeOutStart);
            }
            if (distance < fadeInStart) {
                return 0.0f;
            }
            if (distance < fadeInEnd) {
                const float f = (distance - fadeInStart) / (fadeInEnd - fadeInStart);
                return f > 0.0f ? -f : 0.0f;
            }
            return 1.0f;
        }

        bool operator==(const VisibilityRange&) const = default;
    };

    /// Whether a fade value from VisibilityRange::fadeAt draws only part of the instance, which
    /// takes the dithered variant.
    constexpr bool visibilityFadePartial(const float fade) { return fade != 0.0f && fade != 1.0f; }
}
