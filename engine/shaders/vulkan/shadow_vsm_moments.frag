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
    if (!(gl_FragCoord.z >= 0.0 && gl_FragCoord.z <= 1.0)) {
        discard;
    }

    const float VSM_EXPONENT = 5.54;
    float warped = exp(VSM_EXPONENT * (2.0 * gl_FragCoord.z - 1.0));
    outMoments = vec4(warped, warped * warped, 1.0, 1.0);
}
