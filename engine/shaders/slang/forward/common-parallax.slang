// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// ── Parallax occlusion mapping ──
//
// `heightBase` is the height-map value that sits at the level of the geometry
// texels above it stand proud, texels below sink in. 1 treats
// the map as pure depth below the surface; the default 0.5 pivots around mid-grey.
// Implicit-LOD form, for the view march: uniform control flow, mips intact.
float parallaxDepth(float2 uv, float heightBase) {
    return heightBase - heightMap.Sample(materialSampler, uv).r;
}

// Explicit-LOD form, for the self-shadow march inside the light loop.
float parallaxSampleDepth(float2 uv, float heightBase) {
    return heightBase - heightMap.SampleLevel(materialSampler, uv, 0.0).r;
}

float2 parallaxOcclusionMap(float2 uv, float3 viewDirTS, float heightScale, float heightBase) {
    // Adaptive step count: more steps at grazing angles, where parallax shows most.
    const int minSteps = 8;
    const int maxSteps = 32;
    int numSteps = int(lerp(float(maxSteps), float(minSteps), abs(viewDirTS.z)));
    float layerDepth = 1.0 / float(numSteps);

    // The height unit is a TENTH of a uv tile: a factor of 1 is a relief
    // 0.1 uv deep.
    float scale = heightScale * 0.1;
    // UV travelled per unit of depth along the view ray, and the per-layer step.
    float2 uvPerDepth = viewDirTS.xy * scale / (abs(viewDirTS.z) + 1e-5);
    float2 deltaUV = uvPerDepth / float(numSteps);

    // Anything above the base stands proud, so the ray enters at the topmost point
    // the map reaches, its entry UV offset by the distance travelled since. Shifting
    // only the depths moves ray and field together, changing nothing.
    float rise = 1.0 - heightBase;
    float curLayerDepth = -rise;
    float2 curUV = uv + uvPerDepth * rise;
    float curHeight = parallaxDepth(curUV, heightBase);

    for (int i = 0; i < maxSteps; ++i) {
        if (curLayerDepth >= curHeight) break;
        curUV -= deltaUV;
        curHeight = parallaxDepth(curUV, heightBase);
        curLayerDepth += layerDepth;
    }

    // Interpolate between the last two layers for a smooth result.
    float2 prevUV = curUV + deltaUV;
    float afterDepth = curHeight - curLayerDepth;
    float beforeDepth = parallaxDepth(prevUV, heightBase) - (curLayerDepth - layerDepth);
    float weight = afterDepth / (afterDepth - beforeDepth + 1e-6);
    return lerp(curUV, prevUV, weight);
}

// Self-shadowing: march from the displaced point toward the light and accumulate how
// far the height field rises above the ray. Returns a 0..1 visibility factor.
// Explicit LOD because callers sit behind fragment-varying control flow, where
// derivatives are undefined.
float parallaxSelfShadow(float2 uv, float3 lightDirTS, float heightScale,
                         float heightBase, float surfaceDepth, float strength) {
    if (strength <= 0.0 || lightDirTS.z <= 0.0) {
        return 1.0;
    }

    const int numSteps = 16;
    float layerDepth = surfaceDepth / float(numSteps);
    // Same tenth-of-a-tile unit as the view march above.
    float2 deltaUV =
        lightDirTS.xy * (heightScale * 0.1) / (lightDirTS.z + 1e-5) / float(numSteps);

    float occlusion = 0.0;
    float2 curUV = uv;
    float curLayerDepth = surfaceDepth;

    for (int i = 0; i < numSteps; ++i) {
        curUV += deltaUV;
        curLayerDepth -= layerDepth;
        float h = parallaxSampleDepth(curUV, heightBase);
        if (h < curLayerDepth) {
            float blocked = (curLayerDepth - h) * (1.0 - float(i) / float(numSteps));
            occlusion = max(occlusion, blocked);
        }
    }

    // `occlusion` is the deepest the field rises above the ray, in the same [0,1]
    // space the march walks, so it is already a fraction — scaling it by the step
    // count (the obvious-looking normalisation) saturates it to black almost
    // immediately. A factor of 2 gives a visible shadow at strength 1 without
    // crushing the surface.
    return clamp(1.0 - occlusion * strength * 2.0, 0.0, 1.0);
}

// ── Shadow-map filtering: bilinear comparisons and the PCF kernels built on them ──
//
// Metal filters every depth shadow map through a comparison sampler with LINEAR
// filtering: one tap compares the four texels around a point with the receiver and
// blends the four results by the bilinear weights, so a shadow edge moves smoothly
// across a texel. Upstream's PCF1/3/5 kernels are one, four and nine such taps. No
// comparison sampler is bound on Vulkan (the fragment stage is at MoltenVK's
// sampler limit), so the tap is done by hand: the four texels are GATHERED and
// compared here. They are gathered at the exact corner they share, where no sub-texel
// rounding can pick a different 2x2 block, and weighted by the fraction computed
// here, so the texels chosen and the weights given to them cannot disagree.
// Comparing texels directly with uniform weights instead turns every shadow edge into
// a staircase of whole texels — plain to see wherever a texel covers several pixels.
//
// Every shadow tap has an explicit LOD (a gather has none, SampleLevel and
// SampleCmpLevelZero take 0), never .Sample(): the lookups sit inside the per-light loop and
// behind a per-pixel cascade pick, so a 2x2 quad can straddle two atlas quadrants,
// and the implicit-LOD derivatives across it span half the atlas.

// The corner shared by the 2x2 texels a bilinear tap at `uv` reads, and the tap's
// bilinear fraction `f` within them, for a map of `size` texels.
float2 shadowTapCorner(float2 uv, float2 size, out float2 f) {
    float2 t = uv * size - 0.5;
    float2 base = floor(t);
    f = t - base;
    return (base + 1.0) / size;
}

// The bilinear comparison of four gathered depths: lit (1) where receiver <= depth.
// textureGather order: x (i0, j1), y (i1, j1), z (i1, j0), w (i0, j0).
float shadowTapResult(float4 depths, float2 f, float receiver) {
    float4 lit = step(float4(receiver), depths);
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

// One bilinear comparison on a combined-sampler map, and on the clustered atlas (a
// separate image, read through the nearest sampler).
float shadowTap(LocalShadowMap tex, float2 uv, float2 size, float receiver) {
#if defined(VT_TARGET_METAL)
    return shadowCompare(tex, uv, receiver);
#else
    float2 f;
    float2 corner = shadowTapCorner(uv, size, f);
    return shadowTapResult(tex.GatherRed(corner), f, receiver);
#endif
}
float shadowTapAtlas(float2 uv, float2 size, float receiver) {
#if defined(VT_TARGET_METAL)
    return shadowCompare(clusterShadowAtlas, uv, receiver);
#else
    float2 f;
    float2 corner = shadowTapCorner(uv, size, f);
    return shadowTapResult(clusterShadowAtlas.GatherRed(nearestClampSampler, corner), f, receiver);
#endif
}

// Upstream's 3x3 PCF from four bilinear taps: tap i at
// taps[i] with weight weights[i], the weights summing to 1.
void pcf3x3Taps(float2 uv, float2 size, out float2 taps[4], out float weights[4]) {
    float2 texelUv = uv * size;
    float2 inv = 1.0 / size;
    float2 baseFull = floor(texelUv + 0.5);
    float2 st = texelUv + 0.5 - baseFull;
    float2 base = (baseFull - 0.5) * inv;
    float2 w0 = 3.0 - 2.0 * st;
    float2 w1 = 1.0 + 2.0 * st;
    float2 p0 = ((2.0 - st) / w0 - 1.0) * inv + base;
    float2 p1 = (st / w1 + 1.0) * inv + base;
    taps[0] = float2(p0.x, p0.y); weights[0] = w0.x * w0.y / 16.0;
    taps[1] = float2(p1.x, p0.y); weights[1] = w1.x * w0.y / 16.0;
    taps[2] = float2(p0.x, p1.y); weights[2] = w0.x * w1.y / 16.0;
    taps[3] = float2(p1.x, p1.y); weights[3] = w1.x * w1.y / 16.0;
}

// Upstream's 5x5 PCF from nine bilinear taps.
void pcf5x5Taps(float2 uv, float2 size, out float2 taps[9], out float weights[9]) {
    float2 texelUv = uv * size;
    float2 inv = 1.0 / size;
    float2 baseFull = floor(texelUv + 0.5);
    float2 st = texelUv + 0.5 - baseFull;
    float2 base = (baseFull - 0.5) * inv;
    float2 w[3] = float2[3](4.0 - 3.0 * st, float2(7.0), 1.0 + 3.0 * st);
    float2 p[3] = float2[3](((3.0 - 2.0 * st) / w[0] - 2.0) * inv + base,
                        ((3.0 + st) / w[1]) * inv + base,
                        (st / w[2] + 2.0) * inv + base);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            taps[y * 3 + x] = float2(p[x].x, p[y].y);
            weights[y * 3 + x] = w[x].x * w[y].y / 144.0;
        }
    }
}

// 3x3 percentage-closer filter over a local light's map. `receiver` is the (biased)
// light-space depth of the shaded point; a texel is lit when its stored occluder depth
// is no nearer.
float pcf3x3(LocalShadowMap tex, float2 uv, float receiver) {
    float2 size = float2(vtTextureSize(tex, 0));
    float2 taps[4];
    float weights[4];
    pcf3x3Taps(uv, size, taps, weights);
    float sum = 0.0;
    for (int i = 0; i < 4; ++i) {
        sum += weights[i] * shadowTap(tex, taps[i], size, receiver);
    }
    return sum;
}

// The same over the clustered shadow atlas.
float pcf3x3Atlas(float2 uv, float receiver) {
    float2 size = float2(vtTextureSize(clusterShadowAtlas, 0).xy);
    float2 taps[4];
    float weights[4];
    pcf3x3Taps(uv, size, taps, weights);
    float sum = 0.0;
    for (int i = 0; i < 4; ++i) {
        sum += weights[i] * shadowTapAtlas(taps[i], size, receiver);
    }
    return sum;
}

// Cubemap face coordinates with the V term NEGATED: the dominant axis
// of the unnormalized light-to-fragment direction picks the face (+X, -X, +Y, -Y,
// +Z, -Z — the order LightCamera::pointLightRotations renders them), the other two
// axes map to a UV within it, and `tileOffset` is the face's column and row in the
// slot's 3x3 tile grid. The faces are rendered into top-down storage, so v runs
// the other way from a bottom-up convention.
// Mirrored in LightTextureAtlas::cubemapFaceCoordinates,
// which a test holds against the face cameras' real projection.
float2 getCubemapFaceCoordinates(float3 dir, out float2 tileOffset) {
    float3 vAbs = abs(dir);
    float ma;
    float2 uv;
    if (vAbs.z >= vAbs.x && vAbs.z >= vAbs.y) {          // +Z / -Z
        ma = 0.5 / vAbs.z;
        uv = float2(dir.z < 0.0 ? -dir.x : dir.x, dir.y);
        tileOffset = float2(2.0, dir.z < 0.0 ? 1.0 : 0.0);
    } else if (vAbs.y >= vAbs.x) {                        // +Y / -Y
        ma = 0.5 / vAbs.y;
        uv = float2(dir.x, dir.y < 0.0 ? dir.z : -dir.z);
        tileOffset = float2(1.0, dir.y < 0.0 ? 1.0 : 0.0);
    } else {                                              // +X / -X
        ma = 0.5 / vAbs.x;
        uv = float2(dir.x < 0.0 ? dir.z : -dir.z, dir.y);
        tileOffset = float2(0.0, dir.x < 0.0 ? 1.0 : 0.0);
    }
    return uv * ma + 0.5;
}

// Atlas UV of `dir` for an omni light whose slot is `rect` = (x, y, size, edge
// pixels). The face was rendered a few pixels wider than 90 degrees (the edge), so
// its 90-degree content sits inset by the same amount and the UV is inset to meet
// it — a filter kernel at the tile edge then stays inside the tile.
float2 getCubemapAtlasCoordinates(float4 rect, float3 dir) {
    float2 tileOffset;
    float2 uv = getCubemapFaceCoordinates(dir, tileOffset);
    float resolution = float(vtTextureSize(clusterShadowAtlas, 0).x);
    float faceSize = rect.z / 3.0;
    float tileSize = resolution * faceSize;
    float offset = rect.w / max(tileSize, 1.0);
    uv = uv * (1.0 - 2.0 * offset) + offset;
    return uv * faceSize + tileOffset * faceSize + rect.xy;
}

// Visibility of a fragment at `lightToFrag` from a clustered omni light: the face
// stores perspective depth over [near, far], compared against the fragment's own
// dominant-axis distance with the same RELATIVE bias the cubemap path applies
// before the projection. `depthParams` = (near, far, relative bias, unused).
float getShadowOmniClusteredPCF3(float4 rect, float4 depthParams, float3 lightToFrag) {
    float2 uv = getCubemapAtlasCoordinates(rect, lightToFrag);
    float3 absDir = abs(lightToFrag);
    float d = max(absDir.x, max(absDir.y, absDir.z));
    float dBiased = d * (1.0 - depthParams.z);
    float denom = (depthParams.y - depthParams.x) * dBiased;
    float compareValue = depthParams.y * (dBiased - depthParams.x) / max(denom, 1e-6);
    return pcf3x3Atlas(uv, compareValue);
}

