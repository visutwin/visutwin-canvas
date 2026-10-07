// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The depth-of-field blur over a concentric kernel generated in the shader: a centre tap,
// then ring r of R at radius r / R with r * P points (1 + P * R * (R + 1) / 2 taps). The
// step is in UV: the radius is a fraction of a 540-row reference frame, corrected for the
// texture's aspect, so the same blur radius looks the same at every resolution. NEAR: an
// unweighted average of the half-resolution scene. FAR: taps of the CoC-premultiplied far
// texture weighted by the CoC at each tap, normalised by the CoC sum, then divided by this
// pixel's own CoC to undo the premultiply. The aspects arrive as uniforms. Inputs:
// far = 0, coc = 1, near = 2.
import vtquad;
#include "bindings.slang"

struct DofBlurUniforms
{
    float4 radii;   // x = near radius, y = far radius (UV), z = near aspect (h/w, 0 = no near texture), w = far aspect
    float4 rings;   // x = blurRings, y = blurRingPoints
};
VT_DECLARE_QUAD_UNIFORMS(DofBlurUniforms, u)
VT_DECLARE_QUAD_INPUT0(farTexture)
VT_DECLARE_QUAD_INPUT1(cocTexture)
VT_DECLARE_QUAD_INPUT2(nearTexture)

[shader("vertex")]
QuadVarying dofBlurVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 dofBlurFragment(QuadVarying input) : SV_Target0
{
    const float2 uv0 = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    const float2 coc = cocTexture.SampleLevel(uv0, 0.0).rg;
    const float cocFar = coc.r;
    const float cocNear = coc.g;
    const int rings = max(int(u.rings.x), 1);
    const int ringPoints = max(int(u.rings.y), 1);
    float3 sum = float3(0.0, 0.0, 0.0);

    if (cocNear > 0.0001 && u.radii.z > 0.0) {
        const float2 step = cocNear * u.radii.x * float2(u.radii.z, 1.0);
        int count = 1;
        sum += nearTexture.SampleLevel(uv0, 0.0).rgb;
        for (int ring = 1; ring <= rings; ++ring) {
            const float radius = float(ring) / float(rings);
            const int points = ring * ringPoints;
            for (int p = 0; p < points; ++p) {
                const float angle = float(p) * 6.283185307 / float(points);
                const float2 uv = uv0 + step * float2(cos(angle), sin(angle)) * radius;
                sum += nearTexture.SampleLevel(uv, 0.0).rgb;
                ++count;
            }
        }
        sum /= float(count);
    } else if (cocFar > 0.0001) {
        const float2 step = cocFar * u.radii.y * float2(u.radii.w, 1.0);
        float sumCoc = 0.0;
        {
            const float c = cocTexture.SampleLevel(uv0, 0.0).r;
            sum += farTexture.SampleLevel(uv0, 0.0).rgb * c;
            sumCoc += c;
        }
        for (int ring = 1; ring <= rings; ++ring) {
            const float radius = float(ring) / float(rings);
            const int points = ring * ringPoints;
            for (int p = 0; p < points; ++p) {
                const float angle = float(p) * 6.283185307 / float(points);
                const float2 uv = uv0 + step * float2(cos(angle), sin(angle)) * radius;
                const float c = cocTexture.SampleLevel(uv, 0.0).r;
                sum += farTexture.SampleLevel(uv, 0.0).rgb * c;
                sumCoc += c;
            }
        }
        if (sumCoc > 0.0) {
            sum /= sumCoc;
        }
        sum /= cocFar;
    }
    return float4(sum, 1.0);
}
