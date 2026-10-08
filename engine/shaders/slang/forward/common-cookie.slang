// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// ── Light cookies ──
// Every cookie sample below uses ....SampleLevel(0.0). The samples sit inside the
// per-light loop, behind fragment-varying `continue`s, so screen-space derivatives
// there are undefined — and an undefined LOD reads a fully averaged mip, which
// turns any cookie into a flat wash of its own average color.
// DEVIATION: upstream samples cookies with mipmapping.
// A texture the light projects onto the scene, masking its color: a 2D texture
// projected through a spot's beam, a cubemap sampled by direction for an omni.
// The 2D cookie transform: the offset rides the projection matrix (the
// renderer pre-multiplies it); the 2x2 transform is applied about the cookie
// centre after the clip test.

// A cookie channel is a 3-character swizzle ('rgb', or a single channel
// repeated, e.g. 'a' → 'aaa'). CookieChannel carries those five options.
float3 cookieChannelValue(float4 texel, uint channel) {
    if (channel == 1u) return float3(texel.r);
    if (channel == 2u) return float3(texel.g);
    if (channel == 3u) return float3(texel.b);
    if (channel == 4u) return float3(texel.a);
    return texel.rgb;
}

// Spot cookie. `clip` is used when the cone
// falloff is disabled and the projection alone must bound the beam.
float3 getCookie2D(int slot, float3 worldPos, uint channel, bool clip) {
    float4x4 transform = (slot == 0) ? lighting.cookieMatrix2D0 : lighting.cookieMatrix2D1;
    float intensity = ((slot == 0) ? lighting.cookieParams2D0 : lighting.cookieParams2D1).x;

    float4 projPos = mul(float4(worldPos, 1.0), transform);
    if (projPos.w <= 0.0) {
        // Behind the light — never lit through the cookie.
        return clip ? float3(0.0) : float3(1.0);
    }
    float2 uv = projPos.xy / projPos.w;
    if (clip && (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)) {
        return float3(0.0);
    }
    // Skipped at identity so a cookie without a transform samples exactly as before.
    float4 xform = lighting.cookieTransform2D[slot];
    if (any(xform != float4(1.0, 0.0, 0.0, 1.0))) {
        uv = mul((uv - float2(0.5)), float2x2(xform.xy, xform.zw)) + float2(0.5);
    }
    float4 texel = (slot == 0) ? cookie2D0.SampleLevel(linearClampSampler, uv, 0.0) : cookie2D1.SampleLevel(linearClampSampler, uv, 0.0);
    return lerp(float3(1.0), cookieChannelValue(texel, channel), intensity);
}

// Omni cookie. The light's world transform rotates the world-space
// light→fragment direction into cookie cube space (multiplying by
// `float3x3(transform)` on the right — the inverse rotation for an orthonormal
// basis). The X flip matches the cube convention used elsewhere in the engine.
float3 getCookieCube(int slot, float3 lightToFrag, uint channel) {
    float4x4 transform = (slot == 0) ? lighting.cookieMatrixCube0 : lighting.cookieMatrixCube1;
    float intensity = ((slot == 0) ? lighting.cookieParamsCube0 : lighting.cookieParamsCube1).x;

    float3 dir = mul(lightToFrag, transpose(float3x3(transform)));
    dir.x *= -1.0;
    float4 texel = (slot == 0) ? cookieCube0.SampleLevel(linearClampSampler, dir, 0.0) : cookieCube1.SampleLevel(linearClampSampler, dir, 0.0);
    return lerp(float3(1.0), cookieChannelValue(texel, channel), intensity);
}

