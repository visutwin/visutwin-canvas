#version 450

// Opacity frontend for the PCF shadow pass, whose only attachment is depth.
//
// Every other depth-only draw runs with NO fragment stage: depth comes out of
// the rasterizer, and a fragment shader that declares a colour output with no
// colour attachment is a MoltenVK hazard that silently drops depth writes. This
// stage therefore declares no output at all — it exists purely to discard the
// fragments the caster's material says are not there. The pipeline attaches it
// only for a shadow shader built with the alpha-test or shadow-dither feature,
// so an ordinary caster still pays nothing.

#include "shadow_opacity.glsl"

void main() {
    applyShadowOpacity();
}
