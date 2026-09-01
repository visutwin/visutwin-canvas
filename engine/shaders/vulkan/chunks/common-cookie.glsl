// ── Light cookies (upstream cookie.js; mirrors common-cookie.metal) ──
// Every cookie sample below uses textureLod(..., 0.0). The samples sit inside the
// per-light loop, behind fragment-varying `continue`s, so screen-space derivatives
// there are undefined — and an undefined LOD reads a fully averaged mip, which
// turns any cookie into a flat wash of its own average color.
// DEVIATION: upstream samples cookies with mipmapping.
// A texture the light projects onto the scene, masking its color: a 2D texture
// projected through a spot's beam, a cubemap sampled by direction for an omni.
// DEVIATION: no cookieTransform / cookieOffset variants.

// Upstream's cookieChannel is a 3-character swizzle ('rgb', or a single channel
// repeated, e.g. 'a' → 'aaa'). CookieChannel carries the same five options.
vec3 cookieChannelValue(vec4 texel, uint channel) {
    if (channel == 1u) return vec3(texel.r);
    if (channel == 2u) return vec3(texel.g);
    if (channel == 3u) return vec3(texel.b);
    if (channel == 4u) return vec3(texel.a);
    return texel.rgb;
}

// Spot cookie. `clip` mirrors upstream's getCookie2DClip, used when the cone
// falloff is disabled and the projection alone must bound the beam.
vec3 getCookie2D(int slot, vec3 worldPos, uint channel, bool clip) {
    mat4 transform = (slot == 0) ? lighting.cookieMatrix2D0 : lighting.cookieMatrix2D1;
    float intensity = ((slot == 0) ? lighting.cookieParams2D0 : lighting.cookieParams2D1).x;

    vec4 projPos = transform * vec4(worldPos, 1.0);
    if (projPos.w <= 0.0) {
        // Behind the light — never lit through the cookie.
        return clip ? vec3(0.0) : vec3(1.0);
    }
    vec2 uv = projPos.xy / projPos.w;
    if (clip && (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)) {
        return vec3(0.0);
    }
    vec4 texel = (slot == 0) ? textureLod(cookie2D0, uv, 0.0) : textureLod(cookie2D1, uv, 0.0);
    return mix(vec3(1.0), cookieChannelValue(texel, channel), intensity);
}

// Omni cookie. The light's world transform rotates the world-space
// light→fragment direction into cookie cube space (upstream's
// `dLightDirNormW * mat3(transform)` — the inverse rotation for an orthonormal
// basis). The X flip matches the cube convention used elsewhere in the engine.
vec3 getCookieCube(int slot, vec3 lightToFrag, uint channel) {
    mat4 transform = (slot == 0) ? lighting.cookieMatrixCube0 : lighting.cookieMatrixCube1;
    float intensity = ((slot == 0) ? lighting.cookieParamsCube0 : lighting.cookieParamsCube1).x;

    vec3 dir = transpose(mat3(transform)) * lightToFrag;
    dir.x *= -1.0;
    vec4 texel = (slot == 0) ? textureLod(cookieCube0, dir, 0.0) : textureLod(cookieCube1, dir, 0.0);
    return mix(vec3(1.0), cookieChannelValue(texel, channel), intensity);
}

