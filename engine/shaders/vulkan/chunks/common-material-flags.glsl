
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
const uint FLAG_OCCLUDE_DIRECT = 1u << 13;   // Material::setOccludeDirect
const uint FLAG_HAS_HEIGHTMAP  = 1u << 17;

// Specular occlusion modes (scene/constants.h SPECOCC_*, Material::setOccludeSpecular).
const uint SPECOCC_NONE = 0u;
const uint SPECOCC_AO = 1u;
const uint SPECOCC_GLOSSDEPENDENT = 2u;

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
    // Upstream's getFalloffInvSquared, which the Metal chunk already matches:
    // 16 / (d^2 + 1), NOT 1 / d^2. The +1 keeps the curve finite at the light and
    // the 16 restores the magnitude that softening costs; without them a light at
    // four units reads 1/16 where upstream reads 16/17, roughly a fifteenth.
    float sqrDist = dist * dist;
    float invSq = 16.0 / (sqrDist + 1.0);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    return invSq * window * window;
}

// Spot cone falloff. SMOOTHSTEP between the two cone cosines, as upstream's
// spot.js and the Metal chunk both do. A plain ramp, squared or not, is dimmer
// through the whole penumbra — a squared ramp gives half the light at the middle
// of it — and only agrees at the two ends.
float getSpotEffect(float innerConeCos, float outerConeCos, float cosAngle) {
    return smoothstep(outerConeCos, innerConeCos, cosAngle);
}

