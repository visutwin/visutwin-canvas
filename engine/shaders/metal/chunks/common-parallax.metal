// ── Parallax occlusion mapping ──
//
// `heightBase` is the height-map value that sits at the level of the geometry
// (upstream's meaning): texels above it stand proud of the polygon and texels below
// sink into it. 1 treats the map as pure depth carved below the surface, which is
// what this port marched before the parameter existed; the default 0.5 pivots the
// relief around mid-grey.
// Implicit-LOD form, for the view march: it runs in uniform control flow at the top
// of the shader, so derivatives are well defined and the height map keeps its mips.
static inline float parallaxDepth(texture2d<float> heightMap, sampler s,
                                  float2 uv, float heightBase) {
    return heightBase - heightMap.sample(s, uv).r;
}

// Explicit-LOD form, for the self-shadow march: that one runs inside the light loop,
// behind fragment-varying control flow, where derivatives are undefined.
static inline float parallaxSampleDepth(texture2d<float> heightMap, sampler s,
                                        float2 uv, float heightBase) {
    // Depth below the geometry, in [base - 1, base]. Negative depths sit ABOVE the
    // polygon.
    return heightBase - heightMap.sample(s, uv, level(0.0)).r;
}

static inline float2 parallaxOcclusionMap(float2 uv, float3 viewDirTS,
                                          texture2d<float> heightMap, sampler s,
                                          float heightScale, float heightBase) {
    // Adaptive step count: more steps at grazing angles where parallax is most visible.
    const int minSteps = 8;
    const int maxSteps = 32;
    const int numSteps = int(mix(float(maxSteps), float(minSteps), abs(viewDirTS.z)));
    const float layerDepth = 1.0 / float(numSteps);

    // Upstream's height unit is a TENTH of a uv tile, so a factor of 1 asks for a
    // relief 0.1 uv deep. Applying that here keeps `heightMapFactor` meaning what it
    // means upstream; used raw, upstream's own tuned value of 0.4 smears the surface
    // into spikes.
    const float scale = heightScale * 0.1;
    // UV travelled per unit of depth along the view ray, and the per-layer step.
    const float2 uvPerDepth = viewDirTS.xy * scale / (abs(viewDirTS.z) + 1e-5);
    const float2 deltaUV = uvPerDepth / float(numSteps);

    // Anything above the base stands proud of the polygon, so the ray enters at the
    // topmost point the map can reach — and because it has been travelling since
    // then, its entry UV is offset laterally by exactly that much. Shifting only the
    // depths moves the ray and the field together and changes nothing at all.
    const float rise = 1.0 - heightBase;
    float curLayerDepth = -rise;
    float2 curUV = uv + uvPerDepth * rise;
    float curHeight = parallaxDepth(heightMap, s, curUV, heightBase);

    // Step through layers until we go below the surface.
    for (int i = 0; i < maxSteps; ++i) {
        if (curLayerDepth >= curHeight) break;
        curUV -= deltaUV;
        curHeight = parallaxDepth(heightMap, s, curUV, heightBase);
        curLayerDepth += layerDepth;
    }

    // Linear interpolation between the last two layers for a smooth result.
    const float2 prevUV = curUV + deltaUV;
    const float afterDepth = curHeight - curLayerDepth;
    const float beforeDepth =
        parallaxDepth(heightMap, s, prevUV, heightBase) - (curLayerDepth - layerDepth);
    const float weight = afterDepth / (afterDepth - beforeDepth + 1e-6);
    return mix(curUV, prevUV, weight);
}

// Self-shadowing: march from the displaced point toward the light and accumulate how
// far the height field rises above the ray. Returns a 0..1 visibility factor.
// `strength` scales the darkening so a material can dial it back without turning the
// whole feature off. Sampling uses an explicit LOD because callers may sit behind
// fragment-varying control flow, where derivatives are undefined.
static inline float parallaxSelfShadow(float2 uv, float3 lightDirTS,
                                       texture2d<float> heightMap, sampler s,
                                       float heightScale, float heightBase,
                                       float surfaceDepth, float strength) {
    if (strength <= 0.0 || lightDirTS.z <= 0.0) {
        // A light at or below the tangent-plane horizon is handled by NdotL, not here.
        return 1.0;
    }

    const int numSteps = 16;
    const float layerDepth = surfaceDepth / float(numSteps);
    // Same tenth-of-a-tile unit as the view march above.
    const float2 deltaUV =
        lightDirTS.xy * (heightScale * 0.1) / (lightDirTS.z + 1e-5) / float(numSteps);

    float occlusion = 0.0;
    float2 curUV = uv;
    float curLayerDepth = surfaceDepth;

    for (int i = 0; i < numSteps; ++i) {
        curUV += deltaUV;
        curLayerDepth -= layerDepth;
        const float h = parallaxSampleDepth(heightMap, s, curUV, heightBase);
        // The field is in front of the ray: this step blocks the light. Later steps
        // are further from the shaded point, so they count for less.
        if (h < curLayerDepth) {
            const float blocked = (curLayerDepth - h) * (1.0 - float(i) / float(numSteps));
            occlusion = max(occlusion, blocked);
        }
    }

    // `occlusion` is the deepest the field rises above the ray, in the same [0,1]
    // space the march walks, so it is already a fraction — scaling it by the step
    // count (the obvious-looking normalisation) saturates it to black almost
    // immediately. A factor of 2 gives a visible shadow at strength 1 without
    // crushing the surface.
    return saturate(1.0 - occlusion * strength * 2.0);
}
