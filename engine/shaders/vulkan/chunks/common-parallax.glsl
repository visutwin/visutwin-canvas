// ── Parallax occlusion mapping (parity with common-parallax.metal) ──
vec2 parallaxOcclusionMap(vec2 uv, vec3 viewDirTS, float heightScale) {
    // Adaptive step count: more steps at grazing angles, where parallax shows most.
    const int minSteps = 8;
    const int maxSteps = 32;
    int numSteps = int(mix(float(maxSteps), float(minSteps), abs(viewDirTS.z)));
    float layerDepth = 1.0 / float(numSteps);

    vec2 deltaUV = viewDirTS.xy * heightScale /
        (abs(viewDirTS.z) + 1e-5) / float(numSteps);

    vec2 curUV = uv;
    float curLayerDepth = 0.0;
    float curHeight = 1.0 - texture(heightMap, curUV).r;

    for (int i = 0; i < maxSteps; ++i) {
        if (curLayerDepth >= curHeight) break;
        curUV -= deltaUV;
        curHeight = 1.0 - texture(heightMap, curUV).r;
        curLayerDepth += layerDepth;
    }

    // Interpolate between the last two layers for a smooth result.
    vec2 prevUV = curUV + deltaUV;
    float afterDepth = curHeight - curLayerDepth;
    float beforeDepth =
        (1.0 - texture(heightMap, prevUV).r) - (curLayerDepth - layerDepth);
    float weight = afterDepth / (afterDepth - beforeDepth + 1e-6);
    return mix(curUV, prevUV, weight);
}

// 3×3 percentage-closer filter: average binary depth comparisons over the
// texel neighbourhood.  `receiver` is the (biased) light-space depth of the
// shaded point; a texel is lit when its stored occluder depth is no nearer.
float pcf3x3(sampler2D tex, vec2 uv, float receiver) {
    vec2 texel = 1.0 / vec2(textureSize(tex, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float occluder = texture(tex, uv + vec2(x, y) * texel).r;
            sum += (receiver <= occluder) ? 1.0 : 0.0;
        }
    }
    return sum / 9.0;
}

// Array-slice variant of pcf3x3, for the clustered spot-shadow atlas.
//
// Takes no sampler argument: clusterShadowAtlas is a sampler-constructor macro
// over a separate image, and GLSL only allows such a constructor at its point of
// use, not as a call argument. There is exactly one array shadow map, so naming
// it directly costs nothing.
//
// DEVIATION: Metal's getShadowPCF3x3Array reconstructs a 3×3 kernel from four
// hardware `sample_compare` taps; this backend has no comparison samplers bound
// anywhere, so it does the nine comparisons directly — same kernel, uniform
// weights instead of the bilinear ones.
float pcf3x3Array(vec2 uv, float slice, float receiver) {
    vec2 texel = 1.0 / vec2(textureSize(clusterShadowAtlas, 0).xy);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float occluder =
                texture(clusterShadowAtlas, vec3(uv + vec2(x, y) * texel, slice)).r;
            sum += (receiver <= occluder) ? 1.0 : 0.0;
        }
    }
    return sum / 9.0;
}

