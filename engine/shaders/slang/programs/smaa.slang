// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.10.2026
//
// SMAA 1x (Subpixel Morphological Anti-Aliasing) in three passes over the scene, one variant
// each:
//   edge     luma edge detection: red marks an edge on a pixel's left, green on its top;
//   weights  for each pixel on an edge, the area the edge before aliasing would cover, looked up
//            from the shape of the edge (searched for its ends, diagonals included) in the area
//            texture; rgba hold the blending weights;
//   blend    each pixel blended with its neighbours by those areas, into the resolved scene.
// Inputs, per variant: edge 0 scene; weights 0 edges, 1 area lookup, 2 search lookup;
// blend 0 scene, 1 weights, 2 edges (for the debug views).
//
// The scene is linear HDR before exposure and tone mapping, so the edges are found on the
// displayed brightness (the exposed luma through a Reinhard curve, gamma encoded) and the
// pixels are blended compressed by a reversible tone mapping of the exposed colour,
// premultiplied by alpha: blended in linear HDR, a bright pixel would dominate its average and
// the edge stay aliased.
//
// The uv runs down the screen with row 0 at the top, as every texture here, so "top" is
// uv - texel.y; the lookup textures are stored the way the reference stores them.
//
// Ported from the reference implementation of SMAA: Copyright (C) 2013 Jorge Jimenez, Jose I.
// Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez; MIT license
// (http://www.iryoku.com/smaa/). See NOTICE.
//
// @variant edge: SMAA_PASS=0
// @variant weights: SMAA_PASS=1
// @variant blend: SMAA_PASS=2
import vtquad;
import vttonemapreversible;
#include "bindings.slang"

struct SmaaUniforms
{
    float4 metrics;     // 1 / width, 1 / height, width, height of the scene
    float4 params;      // threshold, max search steps, max diagonal search steps, corner rounding 0-1
    float4 output;      // x = display exposure, y = debug view (blend: 0 none, 1 edges, 2 weights)
};
VT_DECLARE_QUAD_UNIFORMS(SmaaUniforms, u)

[shader("vertex")]
QuadVarying smaaVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

#if SMAA_PASS == 0

VT_DECLARE_QUAD_INPUT0(colorTexture)

// An edge much weaker than one next to it is not an edge of its own.
static const float kLocalContrastAdaptationFactor = 2.0;

// The displayed brightness: the exposed luma, premultiplied by alpha (a transparent pixel is not
// displayed), compressed by a Reinhard curve and gamma encoded.
float smaaLuma(float2 uv)
{
    const float4 color = colorTexture.SampleLevel(uv, 0.0);
    const float luma = clamp(dot(color.rgb, float3(0.2126, 0.7152, 0.0722)) * (color.a * u.output.x), 0.0, 1.0e4);
    return sqrt(luma / (1.0 + luma));
}

float4 smaaEdge(float2 uv)
{
    const float2 texel = u.metrics.xy;

    // The differences to the left and the top neighbours.
    const float L = smaaLuma(uv);
    const float Lleft = smaaLuma(uv - float2(texel.x, 0.0));
    const float Ltop = smaaLuma(uv - float2(0.0, texel.y));
    float4 delta = float4(abs(L - float2(Lleft, Ltop)), 0.0, 0.0);
    float2 edges = step(float2(u.params.x, u.params.x), delta.xy);

    // Most pixels have no edge and leave the cleared target as it is.
    if (dot(edges, float2(1.0, 1.0)) == 0.0) {
        discard;
    }

    // The differences to the right and the bottom neighbours.
    const float Lright = smaaLuma(uv + float2(texel.x, 0.0));
    const float Lbottom = smaaLuma(uv + float2(0.0, texel.y));
    delta = float4(delta.xy, abs(L - float2(Lright, Lbottom)));
    float2 maxDelta = max(delta.xy, delta.zw);

    // The differences between the left and the left-left, and the top and the top-top.
    const float Lleftleft = smaaLuma(uv - float2(2.0 * texel.x, 0.0));
    const float Ltoptop = smaaLuma(uv - float2(0.0, 2.0 * texel.y));
    delta = float4(delta.xy, abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop)));
    maxDelta = max(maxDelta.xy, delta.zw);
    const float finalDelta = max(maxDelta.x, maxDelta.y);

    // Local contrast adaptation.
    edges = edges * step(float2(finalDelta, finalDelta), kLocalContrastAdaptationFactor * delta.xy);
    return float4(edges, 0.0, 0.0);
}

#elif SMAA_PASS == 1

VT_DECLARE_QUAD_INPUT0(edgesTexture)
VT_DECLARE_QUAD_INPUT1(areaTexture)
VT_DECLARE_QUAD_INPUT2(searchTexture)

static const float kAreaTexMaxDistance = 16.0;
static const float kAreaTexMaxDistanceDiag = 20.0;
// A texel of the area texture: the 160 x 80 subtexture SMAA 1x reads.
static const float2 kAreaTexPixelSize = float2(1.0 / 160.0, 1.0 / 80.0);
static const float2 kSearchTexSize = float2(66.0, 33.0);
static const float2 kSearchTexPackedSize = float2(64.0, 16.0);

float2 sampleEdges(float2 uv)
{
    return edgesTexture.SampleLevel(uv, 0.0).rg;
}

float2 sampleEdgesOffset(float2 uv, float2 offset)
{
    return sampleEdges(uv + offset * u.metrics.xy);
}

// ---- diagonal search ----

// The edges are fetched bilinearly at a 0.25 offset, reading two at once; this unpacks them.
float2 decodeDiagBilinearAccess2(float2 e)
{
    return round(float2(e.r * abs(5.0 * e.r - 5.0 * 0.75), e.g));
}

float4 decodeDiagBilinearAccess4(float4 e)
{
    return round(float4(e.r * abs(5.0 * e.r - 5.0 * 0.75), e.g, e.b * abs(5.0 * e.b - 5.0 * 0.75), e.a));
}

// A diagonal search: the distance and whether the end was found, and the last edges fetched.
struct DiagSearch
{
    float2 result;
    float2 e;
};

// Searches along a diagonal for the end of a line.
DiagSearch searchDiag1(float2 texcoord, float2 dir)
{
    float4 coord = float4(texcoord, -1.0, 1.0);
    const float3 t = float3(u.metrics.xy, 1.0);
    const float maxSteps = u.params.z - 1.0;
    float2 e = float2(0.0, 0.0);
    while (coord.z < maxSteps && coord.w > 0.9) {
        coord = float4(t * float3(dir, 1.0) + coord.xyz, coord.w);
        e = sampleEdges(coord.xy);
        coord.w = dot(e, float2(0.5, 0.5));
    }
    DiagSearch search;
    search.result = coord.zw;
    search.e = e;
    return search;
}

DiagSearch searchDiag2(float2 texcoord, float2 dir)
{
    float4 coord = float4(texcoord, -1.0, 1.0);
    coord.x += 0.25 * u.metrics.x;
    const float3 t = float3(u.metrics.xy, 1.0);
    const float maxSteps = u.params.z - 1.0;
    float2 e = float2(0.0, 0.0);
    while (coord.z < maxSteps && coord.w > 0.9) {
        coord = float4(t * float3(dir, 1.0) + coord.xyz, coord.w);
        // Both edges at once, through bilinear filtering.
        e = decodeDiagBilinearAccess2(sampleEdges(coord.xy));
        coord.w = dot(e, float2(0.5, 0.5));
    }
    DiagSearch search;
    search.result = coord.zw;
    search.e = e;
    return search;
}

// The area below a diagonal line, from the second half of the area texture.
float2 areaDiag(float2 dist, float2 e)
{
    float2 texcoord = float2(kAreaTexMaxDistanceDiag, kAreaTexMaxDistanceDiag) * e + dist;
    texcoord = kAreaTexPixelSize * texcoord + 0.5 * kAreaTexPixelSize;
    texcoord.x += 0.5;
    return areaTexture.SampleLevel(texcoord, 0.0).rg;
}

// The blending weights of the diagonal lines through the pixel.
float2 calculateDiagWeights(float2 texcoord, float2 e)
{
    float2 weights = float2(0.0, 0.0);
    const float4 metrics = u.metrics;

    // The line ends.
    float4 d = float4(0.0, 0.0, 0.0, 0.0);
    if (e.r > 0.0) {
        const DiagSearch search = searchDiag1(texcoord, float2(-1.0, 1.0));
        d.x = search.result.x + (search.e.y > 0.9 ? 1.0 : 0.0);
        d.z = search.result.y;
    }
    const DiagSearch search1 = searchDiag1(texcoord, float2(1.0, -1.0));
    d.y = search1.result.x;
    d.w = search1.result.y;

    if (d.x + d.y > 2.0) {
        // The crossing edges.
        const float4 coords = float4(-d.x + 0.25, d.x, d.y, -d.y - 0.25) * metrics.xyxy + texcoord.xyxy;
        const float4 decoded = decodeDiagBilinearAccess4(float4(sampleEdgesOffset(coords.xy, float2(-1.0, 0.0)),
            sampleEdgesOffset(coords.zw, float2(1.0, 0.0))));
        const float4 c = decoded.yxwz;

        // The crossing edges at each side merged into one value, without the crossing edge
        // where the end of the line was not found.
        float2 cc = float2(2.0, 2.0) * c.xz + c.yw;
        cc = lerp(cc, float2(0.0, 0.0), step(float2(0.9, 0.9), d.zw));

        weights += areaDiag(d.xy, cc);
    }

    // The other diagonal.
    const DiagSearch search2 = searchDiag2(texcoord, float2(-1.0, -1.0));
    d.x = search2.result.x;
    d.z = search2.result.y;
    if (sampleEdgesOffset(texcoord, float2(1.0, 0.0)).r > 0.0) {
        const DiagSearch search3 = searchDiag2(texcoord, float2(1.0, 1.0));
        d.y = search3.result.x + (search3.e.y > 0.9 ? 1.0 : 0.0);
        d.w = search3.result.y;
    } else {
        d.y = 0.0;
        d.w = 0.0;
    }

    if (d.x + d.y > 2.0) {
        const float4 coords = float4(-d.x, -d.x, d.y, d.y) * metrics.xyxy + texcoord.xyxy;
        const float2 right = sampleEdgesOffset(coords.zw, float2(1.0, 0.0));
        const float4 c = float4(
            sampleEdgesOffset(coords.xy, float2(-1.0, 0.0)).g,
            sampleEdgesOffset(coords.xy, float2(0.0, -1.0)).r,
            right.g,
            right.r);
        float2 cc = float2(2.0, 2.0) * c.xz + c.yw;
        cc = lerp(cc, float2(0.0, 0.0), step(float2(0.9, 0.9), d.zw));

        weights += areaDiag(d.xy, cc).gr;
    }

    return weights;
}

// ---- horizontal and vertical search ----

// The length of the edge past the last two pixels fetched, decoded by the search texture from
// their bilinear fetch.
float searchLength(float2 e, float offset)
{
    // The texture is flipped vertically, the left and the right cases taking half of it each.
    float2 scale = kSearchTexSize * float2(0.5, -1.0);
    float2 bias = kSearchTexSize * float2(offset, 1.0);

    // Scale and bias to the texel centres, in the cropped texture.
    scale += float2(-1.0, 1.0);
    bias += float2(0.5, -0.5);
    scale *= 1.0 / kSearchTexPackedSize;
    bias *= 1.0 / kSearchTexPackedSize;

    return searchTexture.SampleLevel(scale * e + bias, 0.0).r;
}

// Searches for the end of a horizontal or vertical line, two pixels at a time, stopping at a
// crossing edge.
float searchXLeft(float2 start, float end)
{
    float2 texcoord = start;
    float2 e = float2(0.0, 1.0);
    while (texcoord.x > end && e.g > 0.8281 && e.r == 0.0) {
        e = sampleEdges(texcoord);
        texcoord = -float2(2.0, 0.0) * u.metrics.xy + texcoord;
    }
    const float offset = -(255.0 / 127.0) * searchLength(e, 0.0) + 3.25;
    return u.metrics.x * offset + texcoord.x;
}

float searchXRight(float2 start, float end)
{
    float2 texcoord = start;
    float2 e = float2(0.0, 1.0);
    while (texcoord.x < end && e.g > 0.8281 && e.r == 0.0) {
        e = sampleEdges(texcoord);
        texcoord = float2(2.0, 0.0) * u.metrics.xy + texcoord;
    }
    const float offset = -(255.0 / 127.0) * searchLength(e, 0.5) + 3.25;
    return -u.metrics.x * offset + texcoord.x;
}

float searchYUp(float2 start, float end)
{
    float2 texcoord = start;
    float2 e = float2(1.0, 0.0);
    while (texcoord.y > end && e.r > 0.8281 && e.g == 0.0) {
        e = sampleEdges(texcoord);
        texcoord = -float2(0.0, 2.0) * u.metrics.xy + texcoord;
    }
    const float offset = -(255.0 / 127.0) * searchLength(e.gr, 0.0) + 3.25;
    return u.metrics.y * offset + texcoord.y;
}

float searchYDown(float2 start, float end)
{
    float2 texcoord = start;
    float2 e = float2(1.0, 0.0);
    while (texcoord.y < end && e.r > 0.8281 && e.g == 0.0) {
        e = sampleEdges(texcoord);
        texcoord = float2(0.0, 2.0) * u.metrics.xy + texcoord;
    }
    const float offset = -(255.0 / 127.0) * searchLength(e.gr, 0.5) + 3.25;
    return -u.metrics.y * offset + texcoord.y;
}

// The area below a horizontal or vertical line, given the distances to its ends and the
// crossing edges there.
float2 area(float2 dist, float e1, float e2)
{
    float2 texcoord = float2(kAreaTexMaxDistance, kAreaTexMaxDistance) * round(4.0 * float2(e1, e2)) + dist;
    texcoord = kAreaTexPixelSize * texcoord + 0.5 * kAreaTexPixelSize;
    return areaTexture.SampleLevel(texcoord, 0.0).rg;
}

// ---- corner detection ----

// Reduces the blending at sharp corners, keeping them sharp: not at all at a corner rounding
// of 1.
float2 detectHorizontalCornerPattern(float2 weights, float4 texcoord, float2 d)
{
    const float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - u.params.w) * leftRight;

    // Less blending for the pixels in the middle of a line.
    rounding /= leftRight.x + leftRight.y;

    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * sampleEdgesOffset(texcoord.xy, float2(0.0, 1.0)).r;
    factor.x -= rounding.y * sampleEdgesOffset(texcoord.zw, float2(1.0, 1.0)).r;
    factor.y -= rounding.x * sampleEdgesOffset(texcoord.xy, float2(0.0, -2.0)).r;
    factor.y -= rounding.y * sampleEdgesOffset(texcoord.zw, float2(1.0, -2.0)).r;
    return weights * saturate(factor);
}

float2 detectVerticalCornerPattern(float2 weights, float4 texcoord, float2 d)
{
    const float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - u.params.w) * leftRight;
    rounding /= leftRight.x + leftRight.y;

    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * sampleEdgesOffset(texcoord.xy, float2(1.0, 0.0)).g;
    factor.x -= rounding.y * sampleEdgesOffset(texcoord.zw, float2(1.0, 1.0)).g;
    factor.y -= rounding.x * sampleEdgesOffset(texcoord.xy, float2(-2.0, 0.0)).g;
    factor.y -= rounding.y * sampleEdgesOffset(texcoord.zw, float2(-2.0, 1.0)).g;
    return weights * saturate(factor);
}

float4 smaaWeights(float2 texcoord)
{
    const float4 metrics = u.metrics;
    const float2 pixcoord = texcoord * metrics.zw;

    // The offsets of the searches, and the ends of their loops.
    const float4 offset0 = metrics.xyxy * float4(-0.25, -0.125, 1.25, -0.125) + texcoord.xyxy;
    const float4 offset1 = metrics.xyxy * float4(-0.125, -0.25, -0.125, 1.25) + texcoord.xyxy;
    const float4 offset2 = metrics.xxyy * (float4(-2.0, 2.0, -2.0, 2.0) * u.params.y) +
        float4(offset0.xz, offset1.yw);

    float4 weights = float4(0.0, 0.0, 0.0, 0.0);
    float2 e = sampleEdges(texcoord);

    // An edge at the top.
    if (e.g > 0.0) {
        // The diagonals first, unless there are no diagonal search steps.
        if (u.params.z > 0.0) {
            const float2 diag = calculateDiagWeights(texcoord, e);
            weights = float4(diag, weights.zw);
        }

        // No diagonal line through the pixel: a horizontal one.
        if (weights.r == -weights.g) {
            const float left = searchXLeft(offset0.xy, offset2.x);
            const float y = offset1.y;

            // The crossing edges at the ends, fetched bilinearly to read both of a pair.
            const float e1 = edgesTexture.SampleLevel(float2(left, y), 0.0).r;
            const float right = searchXRight(offset0.zw, offset2.y);

            // The distances in pixels; their square roots give the area texture its range.
            const float2 d = abs(round(metrics.zz * float2(left, right) - pixcoord.xx));
            const float2 sqrtD = sqrt(d);
            const float e2 = sampleEdgesOffset(float2(right, y), float2(1.0, 0.0)).r;
            const float2 horizontal = detectHorizontalCornerPattern(area(sqrtD, e1, e2),
                float4(left, texcoord.y, right, texcoord.y), d);
            weights = float4(horizontal, weights.zw);
        } else {
            // A diagonal line, which the vertical search would take for edges of its own.
            e.r = 0.0;
        }
    }

    // An edge at the left.
    if (e.r > 0.0) {
        const float up = searchYUp(offset1.xy, offset2.z);
        const float x = offset0.x;

        const float e1 = edgesTexture.SampleLevel(float2(x, up), 0.0).g;
        const float down = searchYDown(offset1.zw, offset2.w);

        const float2 d = abs(round(metrics.ww * float2(up, down) - pixcoord.yy));
        const float2 sqrtD = sqrt(d);
        const float e2 = sampleEdgesOffset(float2(x, down), float2(0.0, 1.0)).g;
        const float2 vertical = detectVerticalCornerPattern(area(sqrtD, e1, e2),
            float4(texcoord.x, up, texcoord.x, down), d);
        weights = float4(weights.xy, vertical);
    }

    return weights;
}

#else

VT_DECLARE_QUAD_INPUT0(colorTexture)
VT_DECLARE_QUAD_INPUT1(blendTexture)
VT_DECLARE_QUAD_INPUT2(edgesTexture)

// Below this alpha the colour of a blended pixel is not divided out of its alpha.
static const float kUnpremultiplyAlpha = 1.0 / 255.0;

// Compressed by the reversible tone mapping of the exposed colour, premultiplied by alpha, so a
// transparent pixel's colour (not displayed) does not tint the pixels blended with it.
float4 smaaCompress(float4 color, float exposure)
{
    return float4(reversibleTonemap(color.rgb * exposure) * color.a, color.a);
}

// The blended colour with its alpha divided out before the (non-linear) expansion; a pixel left
// transparent keeps its colour.
float4 smaaExpand(float4 blended, float4 color, float exposure)
{
    const float3 straight = blended.rgb / max(blended.a, kUnpremultiplyAlpha);
    const float3 rgb = blended.a > kUnpremultiplyAlpha ? reversibleTonemapInverse(straight) / exposure : color.rgb;
    return float4(rgb, blended.a);
}

float4 smaaBlend(float2 uv)
{
    const float2 texel = u.metrics.xy;

    // The debug views: the edges found (red on a pixel's left, green on its top), and how much
    // each pixel blends with its neighbours (red along horizontal edges, green along vertical).
    if (u.output.y > 1.5) {
        const float4 w = blendTexture.SampleLevel(uv, 0.0);
        return float4(w.r + w.g, w.b + w.a, 0.0, 1.0);
    }
    if (u.output.y > 0.5) {
        return float4(edgesTexture.SampleLevel(uv, 0.0).rg, 0.0, 1.0);
    }

    // The pixel's blending weights, with those its right and bottom neighbours hold for it.
    const float4 own = blendTexture.SampleLevel(uv, 0.0);
    const float4 a = float4(
        blendTexture.SampleLevel(uv + float2(texel.x, 0.0), 0.0).a,
        blendTexture.SampleLevel(uv + float2(0.0, texel.y), 0.0).g,
        own.z,
        own.x);

    float4 color = colorTexture.SampleLevel(uv, 0.0);

    // A pixel on an edge blends with its neighbours on either side, along the stronger edge.
    if (dot(a, float4(1.0, 1.0, 1.0, 1.0)) >= 1.0e-5) {
        const bool horizontal = max(a.x, a.z) > max(a.y, a.w);
        const float2 dir = horizontal ? float2(texel.x, 0.0) : float2(0.0, texel.y);
        const float2 fraction = horizontal ? a.xz : a.yw;
        const float2 weight = fraction / dot(fraction, float2(1.0, 1.0));

        const float e = max(u.output.x, 1.0e-6);
        const float4 center = smaaCompress(color, e);
        const float4 positive = smaaCompress(colorTexture.SampleLevel(uv + dir, 0.0), e);
        const float4 negative = smaaCompress(colorTexture.SampleLevel(uv - dir, 0.0), e);
        const float4 blended = weight.x * lerp(center, positive, fraction.x) +
            weight.y * lerp(center, negative, fraction.y);
        color = smaaExpand(blended, color, e);
    }

    return color;
}

#endif

// One entry for the three variants (the bundle finds entries in the source text, before the
// preprocessor), running the pass its variant compiles.
[shader("fragment")]
float4 smaaFragment(QuadVarying input) : SV_Target0
{
#if SMAA_PASS == 0
    return smaaEdge(input.uv);
#elif SMAA_PASS == 1
    return smaaWeights(input.uv);
#else
    return smaaBlend(input.uv);
#endif
}
