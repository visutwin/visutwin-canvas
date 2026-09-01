// ── Environment atlas layout (matches engine bake / Metal common.metal) ──
const float ATLAS_SIZE = 512.0;
const float ATLAS_SEAM = 1.0 / 512.0;

// Equirectangular direction → atlas UV (atan2 azimuth, asin elevation).
vec2 dirToEquirect(vec3 dir) {
    vec2 sph = vec2(atan(dir.x, dir.z), asin(clamp(dir.y, -1.0, 1.0)));
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

vec3 decodeRGBP(vec4 raw) { vec3 c = raw.rgb * (-raw.a * 7.0 + 8.0); return c * c; }
vec3 decodeRGBM(vec4 raw) { vec3 c = (8.0 * raw.a) * raw.rgb; return c * c; }
vec3 srgbToLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }

vec3 decodeEnv(vec4 raw) {
    uint enc = uint(lighting.envParams.z + 0.5);
    if (enc == 1u) return decodeRGBP(raw);
    if (enc == 2u) return decodeRGBM(raw);
    return srgbToLinear(raw.rgb);
}

