// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
// ---------------------------------------------------------------------------
// Procedural Bayer matrix (upstream bayer.js, from shadertoy.com/view/Mlt3z8),
// used by VT_FEATURE_OPACITY_DITHER for ordered-dither transparency.
// ---------------------------------------------------------------------------

// 2x2 bayer matrix [1 2][3 0], p in [0,1]
static inline float bayer2(float2 p) {
    return fmod(2.0 * p.y + p.x + 1.0, 4.0);
}

// 4x4 matrix, p - pixel coordinate
static inline float bayer4(float2 p) {
    const float2 p1 = fmod(p, 2.0);
    const float2 p2 = floor(0.5 * fmod(p, 4.0));
    return 4.0 * bayer2(p1) + bayer2(p2);
}

// 8x8 matrix, p - pixel coordinate
static inline float bayer8(float2 p) {
    const float2 p1 = fmod(p, 2.0);
    const float2 p2 = floor(0.5 * fmod(p, 4.0));
    const float2 p4 = floor(0.25 * fmod(p, 8.0));
    return 4.0 * (4.0 * bayer2(p1) + bayer2(p2)) + bayer2(p4);
}

// 16x16 matrix, p - pixel coordinate
static inline float bayer16(float2 p) {
    const float2 p1 = fmod(p, 2.0);
    const float2 p2 = floor(0.5 * fmod(p, 4.0));
    const float2 p4 = floor(0.25 * fmod(p, 8.0));
    const float2 p8 = floor(0.125 * fmod(p, 16.0));
    return 4.0 * (4.0 * (4.0 * bayer2(p1) + bayer2(p2)) + bayer2(p4)) + bayer2(p8);
}

// Scene color grab (dynamic refraction): trilinear so rough transmission can read
// the blurred mip chain, clamped so edge refraction doesn't wrap.
