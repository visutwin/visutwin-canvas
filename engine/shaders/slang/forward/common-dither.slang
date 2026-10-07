// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// ── Ordered dither (parity with common-dither.metal) ──
// 2x2 bayer matrix [1 2][3 0], p in [0,1]
float bayer2(float2 p) { return glslMod(2.0 * p.y + p.x + 1.0, 4.0); }

// 4x4 matrix, p = pixel coordinate
float bayer4(float2 p) {
    float2 p1 = glslMod(p, 2.0);
    float2 p2 = floor(0.5 * glslMod(p, 4.0));
    return 4.0 * bayer2(p1) + bayer2(p2);
}

// 8x8 matrix, p = pixel coordinate
float bayer8(float2 p) {
    float2 p1 = glslMod(p, 2.0);
    float2 p2 = floor(0.5 * glslMod(p, 4.0));
    float2 p4 = floor(0.25 * glslMod(p, 8.0));
    return 4.0 * (4.0 * bayer2(p1) + bayer2(p2)) + bayer2(p4);
}

// 16x16 matrix, p = pixel coordinate
float bayer16(float2 p) {
    float2 p1 = glslMod(p, 2.0);
    float2 p2 = floor(0.5 * glslMod(p, 4.0));
    float2 p4 = floor(0.25 * glslMod(p, 8.0));
    float2 p8 = floor(0.125 * glslMod(p, 16.0));
    return 4.0 * (4.0 * (4.0 * bayer2(p1) + bayer2(p2)) + bayer2(p4)) + bayer2(p8);
}

// Opacity dither matrices. Must match scene/constants.h :: DitherMode; the
// active mode arrives per material in MaterialData::flags bits 25-27.
static const uint VT_DITHER_BAYER2  = 1u;
static const uint VT_DITHER_BAYER4  = 2u;
static const uint VT_DITHER_BAYER8  = 3u;
static const uint VT_DITHER_BAYER16 = 4u;
static const uint VT_DITHER_BLUENOISE = 5u;
static const uint VT_DITHER_IGNNOISE  = 6u;

// Blue noise texture, G channel (core/math/blueNoise.h), row-major 32x32, four
// texels per uint, lowest byte first.
static const uint kBlueNoise32[256] = { 0x1FCF4CE3u, 0x008047C0u, 0x17873EEDu, 0xD93D94F6u, 0x8239FF13u, 0x30FA216Du, 0x0A925319u, 0x3A238C7Fu, 0x6F330299u, 0xB833D4A0u, 0x74AA6010u, 0xC12D68EEu, 0xAA2976A8u, 0x61904FB8u, 0xF3A07FCDu, 0x62F912B4u, 0x79B9EFC2u, 0x8E5407F9u, 0xCF4AFCC6u, 0x7FB28421u, 0xF1CA6206u, 0xAAEAC50Fu, 0x2DEA4300u, 0xAACC485Cu, 0x138E572Eu, 0x68E3B23Fu, 0xBB339928u, 0xF4D8590Bu, 0x5545E190u, 0x74391E87u, 0x1C6BB8DFu, 0x1690A8E2u, 0xE8D544AFu, 0x8624965Fu, 0xBF7D00D6u, 0x199D44EAu, 0xB399237Du, 0x589CD768u, 0x3D968727u, 0xE53378BFu, 0xCC71099Du, 0x3AF32DC9u, 0x65F04DADu, 0x6EC52F8Eu, 0x0DEBD438u, 0xBE4CFF2Du, 0xFCFF11F0u, 0x66F15205u, 0x2236FF83u, 0x71CC7D51u, 0x1CC05AFFu, 0x5EFF10DBu, 0x71AB4BBDu, 0x187E91C9u, 0x7B6650AAu, 0xCF1F89A6u, 0xDF93C64Cu, 0x9F4604B9u, 0x3E972A0Du, 0xA184B072u, 0x5B178C02u, 0x6604DE3Du, 0x28E237D4u, 0x11B94BEEu, 0x2769B130u, 0xBB64FF89u, 0xA5FAC983u, 0x2845CF66u, 0xEE00F7DBu, 0xF8B723AAu, 0x0DB184A1u, 0x5EDE946Bu, 0x5BEC3C7Fu, 0xE922D1A5u, 0xE0791437u, 0xE7960A24u, 0xC533B556u, 0x2D558649u, 0xBE6D1D6Fu, 0xF8AA34CCu, 0x790FD89Cu, 0x54954030u, 0xB84761D8u, 0x8DBFEE69u, 0x9B6A1E3Eu, 0xA8FFCE09u, 0x8DDDF03Fu, 0x0174275Eu, 0xC74FBB00u, 0xBF6F07FFu, 0x9BFF8FA9u, 0x135C348Bu, 0x16ABDE8Bu, 0x107962DAu, 0x79049FC8u, 0x51EB44FBu, 0x926B222Eu, 0x1A82DEADu, 0xD0EC2EF7u, 0xC8AEFC1Eu, 0x814FF3A0u, 0xBC8F35FFu, 0xAD306051u, 0xD4C4A326u, 0x13FBE4AFu, 0x4C28C258u, 0x553FC569u, 0x07527BA5u, 0xC05D2D6Fu, 0x1A26AF43u, 0xE1D288FFu, 0x690C8269u, 0x369F4573u, 0xE5A18A63u, 0x15749913u, 0x3BD948E4u, 0x0023FFEAu, 0xD8E86DA0u, 0x55B63B7Fu, 0x953BF84Du, 0xCC7F07EFu, 0xB03FF521u, 0x5FBC83D4u, 0xB9698AC4u, 0xEEB27A96u, 0x5C994FD3u, 0x206EA306u, 0x1CD3BB90u, 0x8EB7DA55u, 0x570B7FD7u, 0x4608FB32u, 0x5A0D9BF3u, 0x668D4615u, 0xBF2F1B82u, 0x57FFDA48u, 0xA62D79E6u, 0x4F2C66C4u, 0x7AB7ED15u, 0xAADB3F95u, 0xACE82537u, 0x0FE031FFu, 0x8DB0FA3Bu, 0x38972AEBu, 0x994903ABu, 0xB2FF0B3Du, 0x47C7995Eu, 0x89536CCEu, 0x7F4FC670u, 0x55A1BED1u, 0x1AD978C7u, 0xC60F7F69u, 0xE0F877D5u, 0x82BD9C7Bu, 0xE20F2F40u, 0xBDF2A01Au, 0x3E64D60Au, 0xEC1B7227u, 0x4C6424A6u, 0x8650DDA6u, 0x24B11661u, 0xDD1DE95Bu, 0xAB8BF56Eu, 0x92123B62u, 0x8CBFFF30u, 0x865FBF04u, 0x03BC917Fu, 0xF928B5F0u, 0xCD8EA33Eu, 0xFF507233u, 0x7623CAB2u, 0x48E4CCFFu, 0xE318B678u, 0x32DAF398u, 0x71E5CF0Cu, 0xBE995934u, 0x1F4DED2Fu, 0x94D3A4F4u, 0x4D9D5738u, 0x9D5A802Bu, 0xD0395E23u, 0xAC7C4854u, 0x2CA052F8u, 0x0500D18Cu, 0xB87FC86Cu, 0x64278643u, 0xB617E1EDu, 0xDDAE07D3u, 0x836EF0D0u, 0x66C02B0Eu, 0xB03D801Du, 0xEA4316FFu, 0xDD6326A8u, 0x08C9AD17u, 0xF1408378u, 0x3F186F90u, 0xA9BD008Bu, 0x2090D5FFu, 0x155EC9E2u, 0x91D766C2u, 0x9709FF57u, 0x48FC5C31u, 0x651AAEBEu, 0xAAF834C5u, 0xFF1FE852u, 0x38759F61u, 0x76F195B5u, 0x2A86A34Cu, 0xD5793CB5u, 0x218DE56Du, 0xDD9931D1u, 0x67D77E55u, 0x3797762Du, 0xEA5621E3u, 0xAA2E014Bu, 0x113FF51Eu, 0x55A2C1E1u, 0x7F9D06B2u, 0x0271F753u, 0x0E9AAF2Bu, 0x8BB8D9CDu, 0xAACF06C4u, 0x7FD06C87u, 0x72CA56DDu, 0xF21B8966u, 0xE23ACA80u, 0xE9B48A15u, 0x441DC15Bu, 0x6A0F62F2u, 0xFB7D41F4u, 0x41F2FF24u, 0xEA95BA13u, 0x7FD14105u, 0x76B94C57u, 0x44C844A7u, 0x8E75FF85u, 0x4C8125A7u, 0x639B2EACu, 0x9E5B3714u, 0xAF49288Bu, 0x9667FFA0u, 0x2BFB17EAu, 0x9B650AD7u, 0x7FE10F50u, 0xDCEDC438u, 0xD450DA1Cu, 0x11E7B394u, 0x1D7FF66Eu, 0x0DBC357Eu, 0x7185B071u, 0xDD1CF393u, 0xB5CD2AABu, 0x719F0652u, 0x08E68C5Cu, 0xAC7B45DBu, 0xE039C7D5u, 0xAAD750C7u };

// Dither threshold for a screen position (parity with common-dither.metal).
// `jitter` is the blue-noise jitter xy, zero unless the camera
// jitters for TAA.
float ditherThreshold(uint ditherMode, float2 screenPos, float2 jitter) {
    float2 p = screenPos + jitter;
    float noise;
    if (ditherMode == VT_DITHER_BAYER2) {
        noise = bayer2(floor(glslMod(p, 2.0))) / 4.0;
    } else if (ditherMode == VT_DITHER_BAYER4) {
        noise = bayer4(floor(glslMod(p, 4.0))) / 16.0;
    } else if (ditherMode == VT_DITHER_BAYER16) {
        noise = bayer16(floor(glslMod(p, 16.0))) / 256.0;
    } else if (ditherMode == VT_DITHER_BLUENOISE) {
        // blueNoiseTex32 sampled nearest with repeat: the texel under frac(uv).
        float2 uv = frac(screenPos / 32.0 + jitter);
        uint2 texel = min(uint2(uv * 32.0), uint2(31u));
        uint index = texel.y * 32u + texel.x;
        noise = float((kBlueNoise32[index >> 2u] >> ((index & 3u) * 8u)) & 0xFFu) / 255.0;
    } else if (ditherMode == VT_DITHER_IGNNOISE) {
        // Interleaved gradient noise.
        float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
        noise = frac(magic.z * frac(dot(p, magic.xy)));
    } else {  // VT_DITHER_BAYER8
        noise = bayer8(floor(glslMod(p, 8.0))) / 64.0;
    }
    // The threshold is authored in perceptual (sRGB) space — linearize.
    return pow(noise, 2.2);
}

// True when the fragment loses the dither test and must be discarded.
bool ditherDiscards(uint ditherMode, float2 screenPos, float alpha, float2 jitter) {
    if (alpha <= 0.0) return true;
    if (alpha >= 1.0) return false;
    return alpha < ditherThreshold(ditherMode, screenPos, jitter);
}

// The unjittered test (shadow passes).
bool ditherDiscards(uint ditherMode, float2 screenPos, float alpha) {
    return ditherDiscards(ditherMode, screenPos, alpha, float2(0.0));
}

