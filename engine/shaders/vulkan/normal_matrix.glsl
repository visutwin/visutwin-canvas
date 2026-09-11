// The matrix that transforms a normal, from the matrix that transforms a point.
//
// A normal is carried by the INVERSE TRANSPOSE of the model matrix, not by the
// matrix itself. The two agree only for a rotation and a uniform scale; under
// non-uniform scale they do not, and lighting reads the difference directly — a
// squashed or stretched mesh shades as if it had never been squashed. Every
// Vulkan vertex module passed mat3(model) until 2026-09-11, so that error was
// this backend's alone: Metal has always uploaded the real thing per draw.
//
// What this returns is the COFACTOR matrix, which is the inverse transpose
// multiplied by the determinant. The caller normalizes the result, so the
// determinant's magnitude cancels and only its SIGN survives — which is exactly
// what the Metal backend produces, since it divides the determinant out and then
// multiplies the transformed normal by sign(det). Keeping the two backends equal
// is the point; the cofactor form just gets there without a division, and
// degrades to mat3(model) for the rotations and uniform scales that make up
// almost every draw (a uniform scale s becomes s², which normalize removes).
//
// A mirrored mesh — negative determinant — therefore keeps the normals it would
// have had unmirrored on BOTH backends. See the note in AGENTS.md: upstream
// instead flips the normal and the front face together, and this engine flips
// neither.

mat3 normalMatrixFrom(mat4 model) {
    vec3 c0 = model[0].xyz;
    vec3 c1 = model[1].xyz;
    vec3 c2 = model[2].xyz;
    // The cofactor matrix's columns, built with three cross products.
    return mat3(cross(c1, c2), cross(c2, c0), cross(c0, c1));
}
