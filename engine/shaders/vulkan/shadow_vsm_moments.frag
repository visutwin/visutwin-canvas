#version 450

// EVSM_16F moments writer — the fragment stage for a VSM shadow program, which
// is the one shadow pass that carries a colour attachment (the RGBA16F moments
// target). A PCF shadow pass is depth-only: it runs shadow.frag when the caster
// needs an opacity frontend and no fragment stage at all otherwise.
// Mirrors shadow-fragment.metal: (exp(c·z'), exp(c·z')², 1, 1) with c = 5.54
// and z' = 2·depth − 1. The .z = 1 marks "rendered"; cleared pixels stay
// (0,0,0,0) and synthesize fully-lit moments at sample time.

#include "shadow_opacity.glsl"

layout(location = 0) out vec4 outMoments;

// The pass's view-projection (the same block forward.vert declares; the range is
// visible to the fragment stage).
layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
} vtDraw;

// Twin of shadowDistanceRatio in common-shadow-vsm.metal: a VSM spot light's pass
// stores distance / range, recovering the light (the
// projection centre, where clip x, y and w vanish) and the far plane (from row 2 of
// the GL-style projection) from the view-projection. False for an orthographic pass.
bool shadowDistanceRatio(mat4 vp, vec3 worldPos, out float ratio) {
    vec4 r0 = vec4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    vec4 r1 = vec4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    vec4 r2 = vec4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    vec4 r3 = vec4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    ratio = 0.0;
    if (dot(r3.xyz, r3.xyz) < 1e-12) {
        return false;
    }
    vec3 c01 = cross(r0.xyz, r1.xyz);
    vec3 c13 = cross(r1.xyz, r3.xyz);
    vec3 c30 = cross(r3.xyz, r0.xyz);
    float det = dot(r0.xyz, c13);
    vec3 lightPos = -(r0.w * c13 + r1.w * c30 + r3.w * c01) / det;
    vec3 forwardDir = normalize(r3.xyz);
    float a = -dot(r2.xyz, forwardDir);
    float b = dot(r2.xyz, lightPos) + r2.w;
    float farClip = b / (a + 1.0);
    ratio = min(distance(worldPos, lightPos) / farClip, 0.99999);
    return true;
}

void main() {
    // Same frontend as the depth-only path: a masked caster must not write its
    // moments where its texture says there is no surface.
    applyShadowOpacity();

    // Rasterization of degenerate triangles, which animated (skinned/morphed) meshes can
    // generate, can supply depth outside of the [0, 1] range or even NaN. The exponential
    // warp below turns those into huge values, which the VSM blur then spreads over a large
    // area, generating visible artifacts. The depth range is not clipped for the other shadow
    // types, as they either store depth in the depth buffer, which clamps it, or the error
    // stays confined to individual texels and so is not noticeable.
    // NOTE: written as !(in-range) rather than (out-of-range) so that NaN — for which every
    // comparison is false — also discards.
    float depth = gl_FragCoord.z;
    float distanceRatio;
    if (shadowDistanceRatio(vtDraw.viewProjection, fragWorldPos, distanceRatio)) {
        depth = distanceRatio;
    }
    if (!(depth >= 0.0 && depth <= 1.0)) {
        discard;
    }

    const float VSM_EXPONENT = 5.54;
    float warped = exp(VSM_EXPONENT * (2.0 * depth - 1.0));
    outMoments = vec4(warped, warped * warped, 1.0, 1.0);
}
