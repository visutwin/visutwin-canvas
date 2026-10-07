// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The shadow pass's fragment stages. The vertex stage is the forward family.
//
// The opacity frontend runs before depth is written: a shadow computed without it is the
// shadow of the caster's quad, not of the caster, and every masked material (foliage, a
// fence, a cut-out sign) throws a solid block. The alpha is the SAME product the forward
// pass tests: base-colour alpha times the base-colour map's, times the opacity map's.
//
// Gated on MATERIAL FLAGS, not on shader features: the shadow program is built without
// specialization, so every feature word reads zero here, and the flags carry the same facts
// (bit 0 a base-colour map, bit 1 alpha tested, bit 19 an opacity map, bits 29-31 the
// shadow dither). The C++ side decides from the features which stage the pass runs: the
// moments writer for a VSM light (the one shadow pass with a colour attachment), the
// opacity frontend for a caster that needs it, and no fragment stage at all otherwise.

// Discards the fragment when the caster's material says the surface is not there.
void applyShadowOpacity(ForwardVarying input)
{
    // Neither the alpha test nor the dither is in this variant: the specialised function has
    // no discard at all, so the caster keeps early depth testing (a fragment stage that MAY
    // discard loses it on Apple GPUs whether or not the branch is taken).
    const bool alphaTest = vtFeatureEnabled(VT_FEATURE_ALPHA_TEST_BIT);
    const bool shadowDither = vtFeatureEnabled(VT_FEATURE_SHADOW_DITHER_BIT);
    if (!alphaTest && !shadowDither) {
        return;
    }
    float alpha = material.baseColor.a;
    const bool uv1 = (material.flags & FLAG_BASE_UV1) != 0u;
    if ((material.flags & FLAG_HAS_BASECOLOR) != 0u) {
        const float2 uvBase = applyUvTransform(uv1 ? input.uv1 : input.uv0, material.baseColorTransform0,
            material.baseColorTransform1);
        alpha *= baseColorMap.Sample(uvBase).a;
    }
    if ((material.flags & (1u << 19)) != 0u) {
        const float2 uvOpacity = applyUvTransform(uv1 ? input.uv1 : input.uv0, material.baseColorTransform0,
            material.baseColorTransform1);
        alpha *= opacityMap.Sample(materialSampler, uvOpacity).a;
    }
    if (alphaTest && (material.flags & FLAG_ALPHA_TEST) != 0u && alpha < material.alphaCutoff) {
        discard;
    }
    // A partially opaque caster discards the same screen-space pattern the forward dither
    // does, and so throws a thinned shadow instead of a solid one.
    const uint shadowDitherMode = (material.flags >> 29) & 0x7u;
    if (shadowDither && shadowDitherMode != 0u) {
        const float ditherStrength = material.dispersionParams.y;
        const float ditherAlpha = ditherStrength >= 0.0 ? ditherStrength : alpha;
        if (ditherDiscards(shadowDitherMode, input.position.xy, ditherAlpha)) {
            discard;
        }
    }
}

// A VSM spot light's pass stores distance / range rather than depth: the light (the projection
// centre, where clip x, y and w vanish) and the far plane (from row 2 of the GL-style
// projection) are recovered from the pass's view-projection. False for an orthographic pass.
bool shadowDistanceRatio(float4x4 vp, float3 worldPos, out float ratio)
{
    const float4 r0 = float4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    const float4 r1 = float4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    const float4 r2 = float4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    const float4 r3 = float4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    ratio = 0.0;
    if (dot(r3.xyz, r3.xyz) < 1e-12) {
        return false;
    }
    const float3 c01 = cross(r0.xyz, r1.xyz);
    const float3 c13 = cross(r1.xyz, r3.xyz);
    const float3 c30 = cross(r3.xyz, r0.xyz);
    const float det = dot(r0.xyz, c13);
    const float3 lightPos = -(r0.w * c13 + r1.w * c30 + r3.w * c01) / det;
    const float3 forwardDir = normalize(r3.xyz);
    const float a = -dot(r2.xyz, forwardDir);
    const float b = dot(r2.xyz, lightPos) + r2.w;
    const float farClip = b / (a + 1.0);
    ratio = min(distance(worldPos, lightPos) / farClip, 0.99999);
    return true;
}

[shader("fragment")]
void shadowOpacityFragment(ForwardVarying input)
{
    applyShadowOpacity(input);
}

// EVSM_16F moments: (exp(c z'), exp(c z')^2, 1, 1) with c = 5.54 and z' = 2 depth - 1. The
// .z = 1 marks "rendered"; cleared texels stay (0, 0, 0, 0) and read as fully lit.
[shader("fragment")]
float4 shadowVsmFragment(ForwardVarying input) : SV_Target0
{
    applyShadowOpacity(input);
    float depth = input.position.z;
    float distanceRatio;
    if (shadowDistanceRatio(drawViewProjection(), input.worldPos, distanceRatio)) {
        depth = distanceRatio;
    }
    // A degenerate triangle (animated meshes make them) can rasterise depth outside [0, 1]
    // or NaN, which the exponential warp blows up and the blur then spreads. The NaN test
    // is on the bits: fast math may drop a comparison-based one.
    const uint bits = asuint(depth);
    const bool isNan = (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0u;
    if (isNan || depth < 0.0 || depth > 1.0) {
        discard;
    }
    const float VSM_EXPONENT = 5.54;
    const float warped = exp(VSM_EXPONENT * (2.0 * depth - 1.0));
    return float4(warped, warped * warped, 1.0, 1.0);
}
