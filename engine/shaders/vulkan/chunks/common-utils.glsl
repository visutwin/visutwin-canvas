// ── Environment atlas layout (matches engine bake / Metal common.metal) ──
const float ATLAS_SIZE = 512.0;
const float ATLAS_SEAM = 1.0 / 512.0;

// Equirectangular direction → atlas UV (atan2 azimuth, asin elevation).
vec2 dirToEquirect(vec3 dir) {
    // atan(0, 0) is undefined in GLSL too. This backend happened to return 0 and
    // so read the right texel, but the guard is here so neither backend depends
    // on that. See the matching note in common-utils.metal.
    float azimuth = (dir.x == 0.0 && dir.z == 0.0) ? 0.0 : atan(dir.x, dir.z);
    vec2 sph = vec2(azimuth, asin(clamp(dir.y, -1.0, 1.0)));
    vec2 uv = sph / vec2(6.28318530718, 3.14159265359) + 0.5;
    return vec2(uv.x, 1.0 - uv.y);
}

// Map a [0,1] uv into a packed sub-rect (x,y,w,h), insetting by the 1px seam.
vec2 mapRect(vec2 uv, vec4 rect) {
    return vec2(mix(rect.x + ATLAS_SEAM, rect.x + rect.z - ATLAS_SEAM, uv.x),
                mix(rect.y + ATLAS_SEAM, rect.y + rect.w - ATLAS_SEAM, uv.y));
}

// Lambert irradiance sub-rect: 64×32 region at (128,384).
vec2 mapAmbientUv(vec2 uv) {
    return mapRect(uv, vec4(128.0 / ATLAS_SIZE, 384.0 / ATLAS_SIZE,
                            64.0 / ATLAS_SIZE, 32.0 / ATLAS_SIZE));
}

// Prefiltered roughness mip `level` down the left edge: rect (0, 1-t, t, t/2).
vec2 mapRoughnessUv(vec2 uv, float level) {
    float t = 1.0 / exp2(level);
    return mapRect(uv, vec4(0.0, 1.0 - t, t, t * 0.5));
}

// Sharp ("shiny") mip chain down the diagonal: rect (1-t, 1-t, t, t/2). The
// prefiltered chain above is convolved for roughness; level 0 of THIS one is the
// unconvolved environment, which is what a mirror must sample. Twin of
// mapShinyUv in common-utils.metal — this backend had no shiny path at all, so
// every mirror sampled the roughness rect and came out blurred.
vec2 mapShinyUv(vec2 uv, float level) {
    float t = 1.0 / exp2(level);
    return mapRect(uv, vec4(1.0 - t, 1.0 - t, t, t * 0.5));
}

vec3 decodeRGBP(vec4 raw) { vec3 c = raw.rgb * (-raw.a * 7.0 + 8.0); return c * c; }
vec3 decodeRGBM(vec4 raw) { vec3 c = (8.0 * raw.a) * raw.rgb; return c * c; }
vec3 srgbToLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }

vec3 decodeEnv(vec4 raw) {
    uint enc = uint(lighting.envParams.z + 0.5);
    if (enc == 1u) return decodeRGBP(raw);
    if (enc == 2u) return decodeRGBM(raw);
    return srgbToLinear(raw.rgb);
}

// Environment atlas lookup along a world direction at a roughness: upstream's
// calcReflection (reflectionEnv.js) - the unconvolved shiny rect at a screen-space
// mip for a mirror, the prefiltered chain otherwise, blended toward the next
// roughness level. Decoded, NOT scaled by the environment intensity. Uses screen
// derivatives, so call it from uniform control flow only. The env-atlas
// refraction uses it; the specular block in forward-fragment-ambient spells the
// same lookup inline.
vec3 sampleEnvAtlas(vec3 dir, float roughness) {
    vec2 envUv = dirToEquirect(vec3(-dir.x, dir.y, dir.z));
    float level = clamp(roughness * 5.0, 0.0, 5.0);
    float l0 = floor(level);

    vec2 uvA = envUv * ATLAS_SIZE;
    vec2 uvB = vec2(fract(envUv.x + 0.5), envUv.y) * ATLAS_SIZE;
    float maxd = min(max(dot(dFdx(uvA), dFdx(uvA)), dot(dFdy(uvA), dFdy(uvA))),
                     max(dot(dFdx(uvB), dFdx(uvB)), dot(dFdy(uvB), dFdy(uvB))));
    float shinyLevel = clamp(0.5 * log2(max(maxd, 1e-12)) - 1.0, 0.0, 5.0);
    float shinyL0 = floor(shinyLevel);

    vec3 envA;
    if (l0 == 0.0) {
        envA = mix(decodeEnv(texture(envAtlas, mapShinyUv(envUv, shinyL0))),
                   decodeEnv(texture(envAtlas, mapShinyUv(envUv, shinyL0 + 1.0))),
                   shinyLevel - shinyL0);
    } else {
        envA = decodeEnv(texture(envAtlas, mapRoughnessUv(envUv, l0)));
    }
    vec3 envB = decodeEnv(texture(envAtlas, mapRoughnessUv(envUv, l0 + 1.0)));
    return mix(envA, envB, level - l0);
}

