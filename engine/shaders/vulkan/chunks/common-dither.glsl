// ── Ordered dither (parity with common-dither.metal, upstream bayer.js) ──
// 2x2 bayer matrix [1 2][3 0], p in [0,1]
float bayer2(vec2 p) { return mod(2.0 * p.y + p.x + 1.0, 4.0); }

// 4x4 matrix, p = pixel coordinate
float bayer4(vec2 p) {
    vec2 p1 = mod(p, 2.0);
    vec2 p2 = floor(0.5 * mod(p, 4.0));
    return 4.0 * bayer2(p1) + bayer2(p2);
}

// 8x8 matrix, p = pixel coordinate
float bayer8(vec2 p) {
    vec2 p1 = mod(p, 2.0);
    vec2 p2 = floor(0.5 * mod(p, 4.0));
    vec2 p4 = floor(0.25 * mod(p, 8.0));
    return 4.0 * (4.0 * bayer2(p1) + bayer2(p2)) + bayer2(p4);
}

// 16x16 matrix, p = pixel coordinate
float bayer16(vec2 p) {
    vec2 p1 = mod(p, 2.0);
    vec2 p2 = floor(0.5 * mod(p, 4.0));
    vec2 p4 = floor(0.25 * mod(p, 8.0));
    vec2 p8 = floor(0.125 * mod(p, 16.0));
    return 4.0 * (4.0 * (4.0 * bayer2(p1) + bayer2(p2)) + bayer2(p4)) + bayer2(p8);
}

// Opacity dither matrices. Must match scene/constants.h :: DitherMode; the
// active mode arrives per material in MaterialData::flags bits 25-27.
const uint VT_DITHER_BAYER2  = 1u;
const uint VT_DITHER_BAYER4  = 2u;
const uint VT_DITHER_BAYER8  = 3u;
const uint VT_DITHER_BAYER16 = 4u;

// Ordered-dither threshold for a screen position (parity with common-dither.metal).
float ditherThreshold(uint ditherMode, vec2 screenPos) {
    float noise;
    if (ditherMode == VT_DITHER_BAYER2) {
        noise = bayer2(floor(mod(screenPos, 2.0))) / 4.0;
    } else if (ditherMode == VT_DITHER_BAYER4) {
        noise = bayer4(floor(mod(screenPos, 4.0))) / 16.0;
    } else if (ditherMode == VT_DITHER_BAYER16) {
        noise = bayer16(floor(mod(screenPos, 16.0))) / 256.0;
    } else {  // VT_DITHER_BAYER8
        noise = bayer8(floor(mod(screenPos, 8.0))) / 64.0;
    }
    // The threshold is authored in perceptual (sRGB) space — linearize.
    return pow(noise, 2.2);
}

// True when the fragment loses the ordered-dither test and must be discarded.
bool ditherDiscards(uint ditherMode, vec2 screenPos, float alpha) {
    if (alpha <= 0.0) return true;
    if (alpha >= 1.0) return false;
    return alpha < ditherThreshold(ditherMode, screenPos);
}

