// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
// ---------------------------------------------------------------------------
// Procedural Bayer matrix (from shadertoy.com/view/Mlt3z8),
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

// Blue noise, G channel (core/math/blueNoise.h), row-major 32x32.
constant uchar kBlueNoise32[1024] = {
    227, 76, 207, 31, 192, 71, 128, 0, 237, 62, 135, 23, 246, 148, 61, 217, 19, 255, 57, 130, 109, 33, 250, 48, 25, 83, 146, 10, 127, 140, 35, 58,
    153, 2, 51, 111, 160, 212, 51, 184, 16, 96, 170, 116, 238, 104, 45, 193, 168, 118, 41, 170, 184, 79, 144, 97, 205, 127, 160, 243, 180, 18, 249, 98,
    194, 239, 185, 121, 249, 7, 84, 142, 198, 252, 74, 207, 33, 132, 178, 127, 6, 98, 202, 241, 15, 197, 234, 170, 0, 67, 234, 45, 92, 72, 204, 170,
    46, 87, 142, 19, 63, 178, 227, 104, 40, 153, 51, 187, 11, 89, 216, 244, 144, 225, 69, 85, 135, 30, 57, 116, 223, 184, 107, 28, 226, 168, 144, 22,
    175, 68, 213, 232, 95, 150, 36, 134, 214, 0, 125, 191, 234, 68, 157, 25, 125, 35, 153, 179, 104, 215, 156, 88, 39, 135, 150, 61, 191, 120, 51, 229,
    157, 9, 113, 204, 201, 45, 243, 58, 173, 77, 240, 101, 142, 47, 197, 110, 56, 212, 235, 13, 45, 255, 76, 190, 240, 17, 255, 252, 5, 82, 241, 102,
    131, 255, 54, 34, 81, 125, 204, 113, 255, 90, 192, 28, 219, 16, 255, 94, 189, 75, 171, 113, 201, 145, 126, 24, 170, 80, 102, 123, 166, 137, 31, 207,
    76, 198, 147, 223, 185, 4, 70, 159, 13, 42, 151, 62, 114, 176, 132, 161, 2, 140, 23, 91, 61, 222, 4, 102, 212, 55, 226, 40, 238, 75, 185, 17,
    48, 177, 105, 39, 137, 255, 100, 187, 131, 201, 250, 165, 102, 207, 69, 40, 219, 247, 0, 238, 170, 35, 183, 248, 161, 132, 177, 13, 107, 148, 222, 94,
    127, 60, 236, 91, 165, 209, 34, 233, 55, 20, 121, 224, 36, 10, 150, 231, 86, 181, 51, 197, 73, 134, 85, 45, 111, 29, 109, 190, 204, 52, 170, 248,
    156, 216, 15, 121, 48, 64, 149, 84, 216, 97, 71, 184, 105, 238, 191, 141, 62, 30, 106, 155, 9, 206, 255, 168, 63, 240, 221, 141, 94, 39, 116, 1,
    0, 187, 79, 199, 255, 7, 111, 191, 169, 143, 255, 155, 139, 52, 92, 19, 139, 222, 171, 22, 218, 98, 121, 16, 200, 159, 4, 121, 251, 68, 235, 81,
    46, 34, 107, 146, 173, 222, 130, 26, 247, 46, 236, 208, 30, 252, 174, 200, 160, 243, 79, 129, 255, 53, 143, 188, 81, 96, 48, 173, 38, 163, 196, 212,
    175, 228, 251, 19, 88, 194, 40, 76, 105, 197, 63, 85, 165, 123, 82, 7, 111, 45, 93, 192, 67, 175, 38, 26, 255, 136, 210, 225, 105, 130, 12, 105,
    115, 69, 159, 54, 99, 138, 161, 229, 19, 153, 116, 21, 228, 72, 217, 59, 234, 255, 35, 0, 160, 109, 232, 216, 127, 59, 182, 85, 77, 248, 59, 149,
    239, 7, 127, 204, 33, 245, 63, 176, 212, 131, 188, 95, 196, 138, 105, 185, 150, 122, 178, 238, 211, 79, 153, 92, 6, 163, 110, 32, 144, 187, 211, 28,
    85, 218, 183, 142, 215, 127, 11, 87, 50, 251, 8, 70, 243, 155, 13, 90, 21, 70, 141, 102, 130, 27, 47, 191, 72, 218, 255, 87, 230, 121, 45, 166,
    196, 102, 44, 79, 21, 237, 183, 122, 149, 63, 219, 170, 55, 37, 232, 172, 255, 49, 224, 15, 59, 250, 176, 141, 235, 42, 151, 56, 171, 3, 73, 153,
    61, 11, 255, 178, 94, 153, 199, 71, 206, 108, 83, 137, 112, 198, 79, 127, 209, 190, 161, 85, 199, 120, 217, 26, 105, 127, 15, 198, 213, 119, 248, 224,
    123, 156, 189, 130, 64, 47, 15, 226, 26, 160, 242, 189, 10, 214, 100, 62, 39, 114, 27, 236, 166, 36, 100, 76, 166, 221, 80, 134, 97, 22, 177, 36,
    91, 233, 29, 221, 110, 245, 139, 171, 98, 59, 18, 146, 48, 255, 191, 140, 4, 191, 95, 134, 127, 145, 188, 3, 240, 181, 40, 249, 62, 163, 142, 205,
    51, 114, 80, 255, 178, 202, 35, 118, 255, 204, 228, 72, 120, 182, 24, 227, 152, 243, 218, 50, 12, 207, 229, 113, 52, 89, 153, 190, 47, 237, 77, 31,
    244, 164, 211, 148, 56, 87, 157, 77, 43, 128, 90, 157, 35, 94, 57, 208, 84, 72, 124, 172, 248, 82, 160, 44, 140, 209, 0, 5, 108, 200, 127, 184,
    67, 134, 39, 100, 237, 225, 23, 182, 211, 7, 174, 221, 208, 240, 110, 131, 14, 43, 192, 102, 29, 128, 61, 176, 255, 22, 67, 234, 168, 38, 99, 221,
    23, 173, 201, 8, 120, 131, 64, 241, 144, 111, 24, 63, 139, 0, 189, 169, 255, 213, 144, 32, 226, 201, 94, 21, 194, 102, 215, 145, 87, 255, 9, 151,
    49, 92, 252, 72, 190, 174, 26, 101, 197, 52, 248, 170, 82, 232, 31, 255, 97, 159, 117, 56, 181, 149, 241, 118, 76, 163, 134, 42, 181, 60, 121, 213,
    109, 229, 141, 33, 209, 49, 153, 221, 85, 126, 215, 103, 45, 118, 151, 55, 227, 33, 86, 234, 75, 1, 46, 170, 30, 245, 63, 17, 225, 193, 162, 85,
    178, 6, 157, 127, 83, 247, 113, 2, 43, 175, 154, 14, 205, 217, 184, 139, 196, 6, 207, 170, 135, 108, 208, 127, 221, 86, 202, 114, 102, 137, 27, 242,
    128, 202, 58, 226, 21, 138, 180, 233, 91, 193, 29, 68, 242, 98, 15, 106, 244, 65, 125, 251, 36, 255, 242, 65, 19, 186, 149, 234, 5, 65, 209, 127,
    87, 76, 185, 118, 167, 68, 200, 68, 133, 255, 117, 142, 167, 37, 129, 76, 172, 46, 155, 99, 20, 55, 91, 158, 139, 40, 73, 175, 160, 255, 103, 150,
    234, 23, 251, 43, 215, 10, 101, 155, 80, 15, 225, 127, 56, 196, 237, 220, 28, 218, 80, 212, 148, 179, 231, 17, 110, 246, 127, 29, 126, 53, 188, 13,
    113, 176, 133, 113, 147, 243, 28, 221, 171, 42, 205, 181, 82, 6, 159, 113, 92, 140, 230, 8, 219, 69, 123, 172, 213, 199, 57, 224, 199, 80, 215, 170,
};

// Dither threshold for a screen position under the given DitherMode. `jitter` is a
// per-frame blue-noise offset while the camera jitters for TAA, zero otherwise. The Bayer matrices are normalized by their
// cell count so the result stays in [0, 1); every pattern is then linearized, as it is
// authored in perceptual (sRGB) space.
static inline float ditherThreshold(uint ditherMode, float2 screenPos, float2 jitter) {
    const float2 p = screenPos + jitter;
    float noise = 0.0;
    switch (ditherMode) {
        case VT_DITHER_BAYER2:
            noise = bayer2(floor(fmod(p, 2.0))) / 4.0;
            break;
        case VT_DITHER_BAYER4:
            noise = bayer4(floor(fmod(p, 4.0))) / 16.0;
            break;
        case VT_DITHER_BAYER16:
            noise = bayer16(floor(fmod(p, 16.0))) / 256.0;
            break;
        case VT_DITHER_BLUENOISE: {
            // blueNoiseTex32 sampled nearest with repeat: the texel under fract(uv).
            const float2 uv = fract(screenPos / 32.0 + jitter);
            const uint2 texel = min(uint2(uv * 32.0), uint2(31));
            noise = float(kBlueNoise32[texel.y * 32u + texel.x]) / 255.0;
            break;
        }
        case VT_DITHER_IGNNOISE: {
            // Interleaved gradient noise (Jimenez, "Next Generation Post Processing in
            // Call of Duty: Advanced Warfare").
            const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
            noise = fract(magic.z * fract(dot(p, magic.xy)));
            break;
        }
        default:  // VT_DITHER_BAYER8
            noise = bayer8(floor(fmod(p, 8.0))) / 64.0;
            break;
    }
    return pow(noise, 2.2);
}

// True when the fragment loses the dither test and must be discarded.
static inline bool ditherDiscards(uint ditherMode, float2 screenPos, float alpha, float2 jitter) {
    if (alpha <= 0.0) {
        return true;
    }
    if (alpha >= 1.0) {
        return false;
    }
    return alpha < ditherThreshold(ditherMode, screenPos, jitter);
}

// The unjittered test (shadow passes).
static inline bool ditherDiscards(uint ditherMode, float2 screenPos, float alpha) {
    return ditherDiscards(ditherMode, screenPos, alpha, float2(0.0));
}

// Scene color grab (dynamic refraction): trilinear so rough transmission can read
// the blurred mip chain, clamped so edge refraction doesn't wrap.
