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
        /// For a hardware-instanced mesh: each instance measured from its own origin and faded
        /// or hidden on its own in the vertex stage, instead of the whole mesh from the centre
        /// of its bounds. Ignored for a mesh drawn without instancing.
        bool perInstance = false;

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

    /// What a visibility range does to a mesh instance seen from one viewer.
    struct VisibilityState
    {
        /// The whole instance's fade (VisibilityRange::fadeAt): 1 drawn whole, 0 hidden, a
        /// signed fraction inside a band. Read only while `perInstance` is false.
        float fade = 1.0f;
        /// Each instance decides for itself in the vertex stage (VT_FEATURE_LOD_FADE_INSTANCED).
        bool perInstance = false;

        bool hidden() const { return !perInstance && fade == 0.0f; }
    };

    /// A per-instance range over instances lying `nearest` to `farthest` from the viewer: hidden
    /// when every distance in that interval is hidden, whole when every one is drawn whole (no
    /// per-instance variant needed), otherwise decided per instance.
    inline VisibilityState perInstanceVisibility(const VisibilityRange& range, const float nearest,
        const float farthest)
    {
        if (farthest < range.fadeInStart || nearest >= range.fadeOutEnd) {
            return {0.0f, false};
        }
        if (nearest >= range.fadeInEnd && farthest < range.fadeOutStart) {
            return {1.0f, false};
        }
        return {1.0f, true};
    }

    /// Whether a fade value from VisibilityRange::fadeAt draws only part of the instance, which
    /// takes the dithered variant.
    constexpr bool visibilityFadePartial(const float fade) { return fade != 0.0f && fade != 1.0f; }
}
