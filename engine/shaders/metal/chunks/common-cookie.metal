// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Light cookies — a texture the light projects onto the scene, masking its
// color. Port of upstream cookie.js (getCookie2D / getCookie2DClip /
// getCookieCube). Spot lights project a 2D texture through the beam; omni
// lights sample a cubemap by the light→fragment direction.
//
// DEVIATION: no cookieTransform / cookieOffset variants (upstream's
// getCookie2DXform pair) — the cookie is projected by the light's own frustum
// with no extra 2D rotation/scale.
#if VT_FEATURE_COOKIE_2D || VT_FEATURE_COOKIE_CUBE

// Cookie sampling is unconditionally bilinear + clamped. Clamping is safe for the
// falloff variant too: the projection square circumscribes the light cone, so
// anything beyond [0,1] has already been zeroed by the cone falloff.
//
// Every cookie sample below passes an explicit level(0). The samples sit inside
// the per-light loop, behind fragment-varying `continue`s, so screen-space
// derivatives there are undefined — and an undefined LOD reads a fully averaged
// mip, which turns any cookie into a flat wash of its own average color (which is
// exactly what a heart-shaped cookie did before this was pinned down).
// DEVIATION: upstream samples cookies with mipmapping.
constexpr sampler cookieSampler(coord::normalized, filter::linear,
                                mip_filter::linear, address::clamp_to_edge);

// Upstream's cookieChannel is a 3-character swizzle ('rgb', or a single channel
// repeated, e.g. 'a' → 'aaa'). CookieChannel carries the same five options.
static inline float3 cookieChannelValue(const float4 texel, const uint channel)
{
    switch (channel) {
        case 1u: return float3(texel.r);
        case 2u: return float3(texel.g);
        case 3u: return float3(texel.b);
        case 4u: return float3(texel.a);
        default: return texel.rgb;
    }
}

#endif

#if VT_FEATURE_COOKIE_2D
/// Spot cookie. `transform` is the light's world → cookie-UV projection (the same
/// matrix as its spot shadow VP). `clip` mirrors upstream's getCookie2DClip,
/// used when the cone falloff is disabled and the projection alone must bound the
/// beam — outside it the light contributes nothing.
static inline float3 getCookie2D(texture2d<float> tex, const float4x4 transform,
                                 const float3 worldPos, const float intensity,
                                 const uint channel, const bool clip)
{
    const float4 projPos = transform * float4(worldPos, 1.0);
    if (projPos.w <= 0.0) {
        // Behind the light — never lit through the cookie.
        return clip ? float3(0.0) : float3(1.0);
    }
    const float2 uv = projPos.xy / projPos.w;
    if (clip && (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)) {
        return float3(0.0);
    }
    return mix(float3(1.0), cookieChannelValue(tex.sample(cookieSampler, uv, level(0)), channel), intensity);
}
#endif

#if VT_FEATURE_COOKIE_CUBE
/// Omni cookie. `transform` is the light's world transform; its rotation takes
/// the world-space light→fragment direction into cookie cube space (upstream
/// getCookieCube's `dLightDirNormW * mat3(transform)`, which is the inverse
/// rotation for an orthonormal basis). The X flip matches the cube face
/// convention the rest of the engine samples with (skybox, reflection probe).
static inline float3 getCookieCube(texturecube<float> tex, const float4x4 transform,
                                   const float3 lightToFrag, const float intensity,
                                   const uint channel)
{
    const float3x3 rotation = float3x3(transform[0].xyz, transform[1].xyz, transform[2].xyz);
    float3 dir = transpose(rotation) * lightToFrag;
    dir.x *= -1.0;
    return mix(float3(1.0), cookieChannelValue(tex.sample(cookieSampler, dir, level(0)), channel), intensity);
}
#endif
