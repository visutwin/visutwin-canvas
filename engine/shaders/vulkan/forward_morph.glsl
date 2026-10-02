// Morph targets for the forward vertex stages that morph (forward_morphed.vert,
// forward_skinned_morphed.vert): the delta buffer, the active targets and weights, and
// applyMorph(), which adds each target's position and normal delta for this vertex. The
// Metal twin is applyMorph in common-structs.metal.

layout(std430, set = 4, binding = 1) readonly buffer MorphDeltaData {
    vec4 deltas[];
} morph;
layout(std140, set = 4, binding = 2) uniform MorphParams {
    uvec4 counts;
    uvec4 indices0;
    uvec4 indices1;
    vec4 weights0;
    vec4 weights1;
} morphParams;

void applyMorph(inout vec3 position, inout vec3 normal) {
    for (uint k = 0u; k < morphParams.counts.x; ++k) {
        uint target = k < 4u
            ? morphParams.indices0[k] : morphParams.indices1[k - 4u];
        float weight = k < 4u
            ? morphParams.weights0[k] : morphParams.weights1[k - 4u];
        uint base = (target * morphParams.counts.y + uint(gl_VertexIndex)) * 2u;
        position += weight * morph.deltas[base].xyz;
        normal += weight * morph.deltas[base + 1u].xyz;
    }
}
