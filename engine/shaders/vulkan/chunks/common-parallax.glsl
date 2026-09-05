// ── Parallax occlusion mapping (parity with common-parallax.metal) ──
//
// `heightBase` is the height-map value that sits at the level of the geometry
// (upstream's meaning): texels above it stand proud, texels below sink in. 1 treats
// the map as pure depth below the surface; the default 0.5 pivots around mid-grey.
// Implicit-LOD form, for the view march: uniform control flow, mips intact.
float parallaxDepth(vec2 uv, float heightBase) {
    return heightBase - texture(heightMap, uv).r;
}

// Explicit-LOD form, for the self-shadow march inside the light loop.
float parallaxSampleDepth(vec2 uv, float heightBase) {
    return heightBase - textureLod(heightMap, uv, 0.0).r;
}

vec2 parallaxOcclusionMap(vec2 uv, vec3 viewDirTS, float heightScale, float heightBase) {
    // Adaptive step count: more steps at grazing angles, where parallax shows most.
    const int minSteps = 8;
    const int maxSteps = 32;
    int numSteps = int(mix(float(maxSteps), float(minSteps), abs(viewDirTS.z)));
    float layerDepth = 1.0 / float(numSteps);

    // Upstream's height unit is a TENTH of a uv tile: a factor of 1 is a relief
    // 0.1 uv deep. See the matching note in common-parallax.metal.
    float scale = heightScale * 0.1;
    // UV travelled per unit of depth along the view ray, and the per-layer step.
    vec2 uvPerDepth = viewDirTS.xy * scale / (abs(viewDirTS.z) + 1e-5);
    vec2 deltaUV = uvPerDepth / float(numSteps);

    // Anything above the base stands proud, so the ray enters at the topmost point
    // the map reaches, its entry UV offset by the distance travelled since. Shifting
    // only the depths moves ray and field together, changing nothing.
    float rise = 1.0 - heightBase;
    float curLayerDepth = -rise;
    vec2 curUV = uv + uvPerDepth * rise;
    float curHeight = parallaxDepth(curUV, heightBase);

    for (int i = 0; i < maxSteps; ++i) {
        if (curLayerDepth >= curHeight) break;
        curUV -= deltaUV;
        curHeight = parallaxDepth(curUV, heightBase);
        curLayerDepth += layerDepth;
    }

    // Interpolate between the last two layers for a smooth result.
    vec2 prevUV = curUV + deltaUV;
    float afterDepth = curHeight - curLayerDepth;
    float beforeDepth = parallaxDepth(prevUV, heightBase) - (curLayerDepth - layerDepth);
    float weight = afterDepth / (afterDepth - beforeDepth + 1e-6);
    return mix(curUV, prevUV, weight);
}

// Self-shadowing: march from the displaced point toward the light and accumulate how
// far the height field rises above the ray. Returns a 0..1 visibility factor.
// Explicit LOD because callers sit behind fragment-varying control flow, where
// derivatives are undefined.
float parallaxSelfShadow(vec2 uv, vec3 lightDirTS, float heightScale,
                         float heightBase, float surfaceDepth, float strength) {
    if (strength <= 0.0 || lightDirTS.z <= 0.0) {
        return 1.0;
    }

    const int numSteps = 16;
    float layerDepth = surfaceDepth / float(numSteps);
    // Same tenth-of-a-tile unit as the view march above.
    vec2 deltaUV =
        lightDirTS.xy * (heightScale * 0.1) / (lightDirTS.z + 1e-5) / float(numSteps);

    float occlusion = 0.0;
    vec2 curUV = uv;
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

