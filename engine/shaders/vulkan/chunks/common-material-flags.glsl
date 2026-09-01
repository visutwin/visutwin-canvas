
// Material flag bits (subset of the engine MaterialUniforms flags).
const uint FLAG_ALPHA_TEST   = 1u << 1;
const uint FLAG_HAS_NORMAL   = 1u << 2;
const uint FLAG_DOUBLE_SIDED = 1u << 3;
const uint FLAG_BASE_UV1     = 1u << 4;   // per-map UV-set selection (uv1 when set)
const uint FLAG_NORMAL_UV1   = 1u << 5;
const uint FLAG_HAS_METALROUGH = 1u << 6;
const uint FLAG_METALROUGH_UV1 = 1u << 7;
const uint FLAG_SKYBOX         = 1u << 8;
const uint FLAG_HAS_OCCLUSION  = 1u << 9;
const uint FLAG_OCCLUSION_UV1  = 1u << 10;
const uint FLAG_HAS_EMISSIVE   = 1u << 11;
const uint FLAG_EMISSIVE_UV1   = 1u << 12;
const uint FLAG_HAS_HEIGHTMAP  = 1u << 17;

vec2 applyUvTransform(vec2 uv, vec4 row0, vec4 row1) {
    vec3 h = vec3(uv, 1.0);
    return vec2(dot(h, row0.xyz), dot(h, row1.xyz));
}

// Distance attenuation: inverse-square with a smooth range window, or linear
// falloff when coneParams.z != 0 (matches the engine's falloffModeLinear).
float distanceAttenuation(float dist, float range, float linearFalloff) {
    if (range <= 0.0) {
        return 1.0;
    }
    float t = clamp(dist / range, 0.0, 1.0);
    if (linearFalloff > 0.5) {
        return clamp(1.0 - t, 0.0, 1.0);
    }
    float invSq = 1.0 / max(dist * dist, 1e-4);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    return invSq * window * window;
}

