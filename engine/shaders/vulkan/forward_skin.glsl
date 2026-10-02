// The skin palette for the forward vertex stages that skin (forward_skinned.vert,
// forward_skinned_morphed.vert), and skinMatrix(): the vertex's four bone matrices
// blended by its weights.

layout(std430, set = 4, binding = 0) readonly buffer PaletteData {
    mat4 matrices[];
} palette;

mat4 skinMatrix(vec4 weights, vec4 indices) {
    uvec4 joints = uvec4(indices);
    return weights.x * palette.matrices[joints.x]
         + weights.y * palette.matrices[joints.y]
         + weights.z * palette.matrices[joints.z]
         + weights.w * palette.matrices[joints.w];
}
