// Shadow-pass opacity frontend, shared by shadow.frag (the depth-only PCF pass)
// and shadow_vsm_moments.frag.
//
// Upstream's litShadowMain runs the material frontend before writing depth, and
// a shadow computed without it is the shadow of the caster's quad rather than of
// the caster: every masked material — foliage, a fence, a cut-out sign — threw a
// solid block. The alpha here has to be the SAME product the forward pass tests,
// base-colour alpha times the base-colour texture's alpha, or the shadow's edge
// does not follow the visible one.
//
// Everything here is gated on MATERIAL FLAGS rather than on the shader features
// the rest of the Vulkan tree uses. A shadow program is built without
// specialization constants — all of them read zero in this stage — and the flags
// carry the same two facts anyway: bit 0 is "has a base-colour map" and bit 1 is
// "alpha tested". The C++ side still decides from the features whether this
// stage is in the pipeline at all.

#include "shader_material.glsl"
#include "chunks/common-material-flags.glsl"
#include "chunks/common-dither.glsl"

// The vertex stage is the shared forward one, which writes all seven varyings.
// Declaring the full set — not just the two this stage reads — keeps the
// stage interface matched, which validation checks output by output.
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec2 fragUV0;
layout(location = 3) in vec2 fragUV1;
layout(location = 4) in vec4 fragWorldTangent;
layout(location = 5) in float fragViewDepth;
layout(location = 6) in vec4 fragColor;

// Set 1 binding 0: the caster's base-colour texture. Shadow passes bind a
// material only for a caster that needs this frontend; for anything else the
// binding is the 1x1 white fallback, whose alpha of 1 passes every test.
layout(set = 1, binding = 0) uniform sampler2D shadowBaseColorMap;

// Discards the fragment when the caster's material says the surface is not there.
void applyShadowOpacity() {
    float alpha = material.baseColor.a;
    if ((material.flags & FLAG_HAS_BASECOLOR) != 0u) {
        vec2 uvBase = (material.flags & FLAG_BASE_UV1) != 0u ? fragUV1 : fragUV0;
        uvBase = applyUvTransform(uvBase, material.baseColorTransform0,
            material.baseColorTransform1);
        alpha *= texture(shadowBaseColorMap, uvBase).a;
    }

    if ((material.flags & FLAG_ALPHA_TEST) != 0u && alpha < material.alphaCutoff) {
        discard;
    }

    // Shadow-pass opacity dither (upstream opacityShadowDither, flags bits 29-31,
    // kept independent of the forward dither in bits 25-27). A partially-opaque
    // caster discards the same screen-space Bayer pattern here, so it throws a
    // thinned shadow instead of a solid one. The mode stays a runtime read; the
    // C++ feature bit only decides whether this stage is in the pipeline at all.
    uint shadowDitherMode = (material.flags >> 29) & 0x7u;
    if (shadowDitherMode != 0u) {
        float ditherStrength = material.dispersionParams.y;
        float ditherAlpha = ditherStrength >= 0.0 ? ditherStrength : alpha;
        if (ditherDiscards(shadowDitherMode, gl_FragCoord.xy, ditherAlpha)) {
            discard;
        }
    }
}
