#version 450

// Fullscreen triangle from gl_VertexIndex — no vertex buffer. The fragment
// stage derives UVs from gl_FragCoord, so no varyings are needed and the
// negative-height viewport cannot skew the source↔target texel mapping.

void main() {
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
