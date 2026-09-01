// ── LTC area lights (parity with common-ltc.metal) ──
// Real-Time Polygonal-Light Shading with Linearly Transformed Cosines
// (Heitz, Dupuy, Hill, Neubelt). Rect, disk and sphere shapes.

const float LTC_LUT_SIZE = 64.0;

// LUT coordinate. Metal passes gloss and squares (1 - gloss); gloss is
// 1 - perceptualRoughness there, so this is the same value expressed directly.
vec2 ltcUv(vec3 N, vec3 V, float perceptualRoughness) {
    float lutRoughness = max(perceptualRoughness * perceptualRoughness, 0.001);
    float dotNV = clamp(dot(N, V), 0.0, 1.0);
    vec2 uv = vec2(lutRoughness, sqrt(1.0 - dotNV));
    return uv * ((LTC_LUT_SIZE - 1.0) / LTC_LUT_SIZE) + (0.5 / LTC_LUT_SIZE);
}

// Range window only (upstream getFalloffWindow): a non-punctual light gets its
// physical distance falloff from the LTC form factor, so only the artist-set
// range is applied here. DEVIATION from the Metal chunk: range 0 means "no
// range limit" as everywhere else in this shader, rather than extinguishing
// the light.
float ltcFalloffWindow(float range, vec3 toLight) {
    if (range <= 0.0) {
        return 1.0;
    }
    float sqrDist = dot(toLight, toLight);
    float invRadius = 1.0 / max(range, 1e-4);
    float t = sqrDist * invRadius * invRadius;
    float w = clamp(1.0 - t * t, 0.0, 1.0);
    return w * w;
}

// Form factor of a horizon-clipped rectangle ("Real-Time Area Lighting: a
// Journey from Research to Production", p.102).
float ltcClippedSphereFormFactor(vec3 f) {
    float l = length(f);
    return max((l * l + f.z) / (l + 1.0), 0.0);
}

vec3 ltcEdgeVectorFormFactor(vec3 v1, vec3 v2) {
    float x = dot(v1, v2);
    float y = abs(x);
    // rational polynomial approximation to theta / sin(theta) / 2PI
    float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float b = 3.4175940 + (4.1616724 + y) * y;
    float v = a / b;
    float thetaSintheta = (x > 0.0)
        ? v
        : 0.5 * inversesqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * thetaSintheta;
}

// LTC integral of a rect light (corners p0..p3, ccw) over the cosine
// distribution transformed by mInv, as seen from surface point P.
float ltcEvaluateRect(vec3 N, vec3 V, vec3 P, mat3 mInv,
                      vec3 p0, vec3 p1, vec3 p2, vec3 p3) {
    vec3 v1 = p1 - p0;
    vec3 v2 = p3 - p0;
    vec3 lightNormal = cross(v1, v2);
    float factor = sign(-dot(lightNormal, P - p0));

    // orthonormal basis around N
    vec3 T1 = normalize(V - N * dot(V, N));
    vec3 T2 = factor * cross(N, T1); // negated from the paper due to handedness

    mat3 mat = mInv * transpose(mat3(T1, T2, N));

    // transform rect corners and project onto sphere
    vec3 c0 = normalize(mat * (p0 - P));
    vec3 c1 = normalize(mat * (p1 - P));
    vec3 c2 = normalize(mat * (p2 - P));
    vec3 c3 = normalize(mat * (p3 - P));

    vec3 vectorFormFactor = vec3(0.0);
    vectorFormFactor += ltcEdgeVectorFormFactor(c0, c1);
    vectorFormFactor += ltcEdgeVectorFormFactor(c1, c2);
    vectorFormFactor += ltcEdgeVectorFormFactor(c2, c3);
    vectorFormFactor += ltcEdgeVectorFormFactor(c3, c0);

    return ltcClippedSphereFormFactor(vectorFormFactor);
}

// Extended cubic solver from "How to solve a cubic equation, revisited"
// (http://momentsingraphics.de/?p=105) — upstream SolveCubic.
vec3 ltcSolveCubic(vec4 coefficient) {
    const float pi = 3.14159;
    coefficient.xyz /= coefficient.w;
    coefficient.yz /= 3.0;

    float B = coefficient.z;
    float C = coefficient.y;
    float D = coefficient.x;

    vec3 delta = vec3(
        -coefficient.z * coefficient.z + coefficient.y,
        -coefficient.y * coefficient.z + coefficient.x,
        dot(vec2(coefficient.z, -coefficient.y), coefficient.xy));

    float discriminant = dot(vec2(4.0 * delta.x, -delta.y), delta.zy);

    vec2 xlc, xsc;

    // Algorithm A
    {
        float C_a = delta.x;
        float D_a = -2.0 * B * delta.x + delta.y;

        float theta = atan(sqrt(discriminant), -D_a) / 3.0;

        float x_1a = 2.0 * sqrt(-C_a) * cos(theta);
        float x_3a = 2.0 * sqrt(-C_a) * cos(theta + (2.0 / 3.0) * pi);

        float xl = ((x_1a + x_3a) > 2.0 * B) ? x_1a : x_3a;
        xlc = vec2(xl - B, 1.0);
    }

    // Algorithm D
    {
        float C_d = delta.z;
        float D_d = -D * delta.y + 2.0 * C * delta.z;

        float theta = atan(D * sqrt(discriminant), -D_d) / 3.0;

        float x_1d = 2.0 * sqrt(-C_d) * cos(theta);
        float x_3d = 2.0 * sqrt(-C_d) * cos(theta + (2.0 / 3.0) * pi);

        float xs = (x_1d + x_3d < 2.0 * C) ? x_1d : x_3d;
        xsc = vec2(-D, xs + C);
    }

    float E = xlc.y * xsc.y;
    float F = -xlc.x * xsc.y - xlc.y * xsc.x;
    float G = xlc.x * xsc.x;

    vec2 xmc = vec2(C * F - B * G, -B * F + C * E);

    vec3 root = vec3(xsc.x / xsc.y, xmc.x / xmc.y, xlc.x / xlc.y);

    if (root.x < root.y && root.x < root.z) {
        root.xyz = root.yxz;
    } else if (root.z < root.x && root.z < root.y) {
        root.xyz = root.xzy;
    }
    return root;
}

// LTC integral of a disk light inscribed in the quad p0/p1/p2 (upstream
// LTC_EvaluateDisk). LUT2's w channel holds the horizon-clipped sphere scale.
float ltcEvaluateDisk(vec3 N, vec3 V, vec3 P, mat3 mInv,
                      vec3 p0, vec3 p1, vec3 p2) {
    // orthonormal basis around N
    vec3 T1 = normalize(V - N * dot(V, N));
    vec3 T2 = cross(N, T1);

    // rotate area light into the (T1, T2, N) basis
    mat3 R = transpose(mat3(T1, T2, N));
    vec3 L0 = R * (p0 - P);
    vec3 L1 = R * (p1 - P);
    vec3 L2 = R * (p2 - P);

    // init ellipse
    vec3 C  = mInv * (0.5 * (L0 + L2));
    vec3 V1 = mInv * (0.5 * (L1 - L2));
    vec3 V2 = mInv * (0.5 * (L1 - L0));

    // eigenvectors of the ellipse
    float a, b;
    float d11 = dot(V1, V1);
    float d22 = dot(V2, V2);
    float d12 = dot(V1, V2);
    if (abs(d12) / sqrt(d11 * d22) > 0.0001) {
        float tr = d11 + d22;
        float det = -d12 * d12 + d11 * d22;

        // use the sqrt matrix to solve for eigenvalues
        det = sqrt(det);
        float u = 0.5 * sqrt(tr - 2.0 * det);
        float v = 0.5 * sqrt(tr + 2.0 * det);
        float e_max = (u + v) * (u + v);
        float e_min = (u - v) * (u - v);

        vec3 V1_, V2_;
        if (d11 > d22) {
            V1_ = d12 * V1 + (e_max - d11) * V2;
            V2_ = d12 * V1 + (e_min - d11) * V2;
        } else {
            V1_ = d12 * V2 + (e_max - d22) * V1;
            V2_ = d12 * V2 + (e_min - d22) * V1;
        }

        a = 1.0 / e_max;
        b = 1.0 / e_min;
        V1 = normalize(V1_);
        V2 = normalize(V2_);
    } else {
        a = 1.0 / dot(V1, V1);
        b = 1.0 / dot(V2, V2);
        V1 *= sqrt(a);
        V2 *= sqrt(b);
    }

    vec3 V3 = normalize(cross(V1, V2));
    if (dot(C, V3) < 0.0) {
        V3 *= -1.0;
    }

    float L  = dot(V3, C);
    float x0 = dot(V1, C) / L;
    float y0 = dot(V2, C) / L;

    a *= L * L;
    b *= L * L;

    float c0 = a * b;
    float c1 = a * b * (1.0 + x0 * x0 + y0 * y0) - a - b;
    float c2 = 1.0 - a * (1.0 + x0 * x0) - b * (1.0 + y0 * y0);
    float c3 = 1.0;

    vec3 roots = ltcSolveCubic(vec4(c0, c1, c2, c3));
    float e1 = roots.x;
    float e2 = roots.y;
    float e3 = roots.z;

    vec3 avgDir = vec3(a * x0 / (a - e2), b * y0 / (b - e2), 1.0);

    mat3 rotate = mat3(V1, V2, V3);
    avgDir = normalize(rotate * avgDir);

    float L1_ = sqrt(-e2 / e3);
    float L2_ = sqrt(-e2 / e1);

    float formFactor = max(0.0,
        L1_ * L2_ * inversesqrt((1.0 + L1_ * L1_) * (1.0 + L2_ * L2_)));

    // tabulated horizon-clipped sphere
    vec2 uv = vec2(avgDir.z * 0.5 + 0.5, formFactor);
    uv = uv * ((LTC_LUT_SIZE - 1.0) / LTC_LUT_SIZE) + (0.5 / LTC_LUT_SIZE);

    float scale = textureLod(areaLightLut2, uv, 0.0).w;
    float result = formFactor * scale;

    // upstream FixNan: the disk evaluator rarely produces NaNs; zero them
    // before they spread through bloom/DOF blurs.
    return isnan(result) ? 0.0 : result;
}

