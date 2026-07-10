#version 450

// EVSM_16F moments writer — substituted as the fragment stage for shadow
// programs (createShader detects "program-shadow"). PCF shadow passes are
// depth-only and omit the fragment stage entirely; this shader only runs when
// the shadow render target carries the RGBA16F moments color attachment.
// Mirrors shadow-fragment.metal: (exp(c·z'), exp(c·z')², 1, 1) with c = 5.54
// and z' = 2·depth − 1. The .z = 1 marks "rendered"; cleared pixels stay
// (0,0,0,0) and synthesize fully-lit moments at sample time.

layout(location = 0) out vec4 outMoments;

void main() {
    const float VSM_EXPONENT = 5.54;
    float warped = exp(VSM_EXPONENT * (2.0 * gl_FragCoord.z - 1.0));
    outMoments = vec4(warped, warped * warped, 1.0, 1.0);
}
