// The matrix that transforms a normal, from the matrix that transforms a point.
//
// A normal is carried by the INVERSE TRANSPOSE of the model matrix, not by the
// matrix itself. The two agree only for a rotation and a uniform scale; under
// non-uniform scale they do not, and lighting reads the difference directly — a
// squashed or stretched mesh shades as if it had never been squashed. Every
// Vulkan vertex module passed mat3(model) until 2026-09-11, so that error was
// this backend's alone: Metal has always uploaded the real thing per draw.
//
// What this returns is the cofactor matrix times the sign of the determinant,
// which is the inverse transpose scaled by |det| — and the caller normalizes, so
// a positive factor cancels and this IS the inverse transpose. Getting there
// through cofactors costs three cross products and no division; going through
// det also keeps the sign, which a mirrored mesh needs: its normals must flip
// with its surface, the same way upstream's matrix_normal flips them. The
// renderer flips which face is culled for the same meshes (applyNodeScaleFlip),
// and the two halves only make sense together.

mat3 normalMatrixFrom(mat4 model) {
    vec3 c0 = model[0].xyz;
    vec3 c1 = model[1].xyz;
    vec3 c2 = model[2].xyz;
    // The cofactor matrix's columns, built with three cross products. Its first
    // column doubles as the term the determinant needs, so the sign is nearly free.
    vec3 cof0 = cross(c1, c2);
    float detSign = dot(c0, cof0) < 0.0 ? -1.0 : 1.0;
    return detSign * mat3(cof0, cross(c2, c0), cross(c0, c1));
}
