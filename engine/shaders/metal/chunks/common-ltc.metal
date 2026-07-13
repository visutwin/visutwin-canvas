// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
// ---------------------------------------------------------------------------
// LTC area lights — Real-Time Polygonal-Light Shading with Linearly
// Transformed Cosines (Heitz, Dupuy, Hill, Neubelt).
// Port of upstream ltc.js: rect, disk and sphere shapes.
// ---------------------------------------------------------------------------

constexpr sampler ltcLutSampler(filter::linear, mip_filter::none, address::clamp_to_edge);

static inline float2 ltcUv(float3 N, float3 V, float gloss)
{
    const float LUT_SIZE = 64.0;
    const float roughness = max((1.0 - gloss) * (1.0 - gloss), 0.001);
    const float dotNV = saturate(dot(N, V));
    const float2 uv = float2(roughness, sqrt(1.0 - dotNV));
    return uv * ((LUT_SIZE - 1.0) / LUT_SIZE) + (0.5 / LUT_SIZE);
}

// An approximation of the form factor of a horizon-clipped rectangle
// ("Real-Time Area Lighting: a Journey from Research to Production", p.102).
static inline float ltcClippedSphereFormFactor(float3 f)
{
    const float l = length(f);
    return max((l * l + f.z) / (l + 1.0), 0.0);
}

static inline float3 ltcEdgeVectorFormFactor(float3 v1, float3 v2)
{
    const float x = dot(v1, v2);
    const float y = abs(x);
    // rational polynomial approximation to theta / sin(theta) / 2PI
    const float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    const float b = 3.4175940 + (4.1616724 + y) * y;
    const float v = a / b;
    const float thetaSintheta = (x > 0.0) ? v : 0.5 * rsqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * thetaSintheta;
}

// Evaluates the LTC integral of a rect light (corners p0..p3, ccw) over the
// cosine distribution transformed by mInv, as seen from surface point P.
static inline float ltcEvaluateRect(float3 N, float3 V, float3 P, float3x3 mInv,
    float3 p0, float3 p1, float3 p2, float3 p3)
{
    const float3 v1 = p1 - p0;
    const float3 v2 = p3 - p0;
    const float3 lightNormal = cross(v1, v2);
    const float factor = sign(-dot(lightNormal, P - p0));

    // construct orthonormal basis around N
    const float3 T1 = normalize(V - N * dot(V, N));
    const float3 T2 = factor * cross(N, T1); // negated from paper due to handedness

    // rows of transpose(mat3(T1, T2, N)) then transformed by mInv
    const float3x3 mat = mInv * transpose(float3x3(T1, T2, N));

    // transform rect corners and project onto sphere
    const float3 c0 = normalize(mat * (p0 - P));
    const float3 c1 = normalize(mat * (p1 - P));
    const float3 c2 = normalize(mat * (p2 - P));
    const float3 c3 = normalize(mat * (p3 - P));

    float3 vectorFormFactor = float3(0.0);
    vectorFormFactor += ltcEdgeVectorFormFactor(c0, c1);
    vectorFormFactor += ltcEdgeVectorFormFactor(c1, c2);
    vectorFormFactor += ltcEdgeVectorFormFactor(c2, c3);
    vectorFormFactor += ltcEdgeVectorFormFactor(c3, c0);

    return ltcClippedSphereFormFactor(vectorFormFactor);
}

// An extended version of the cubic solver from "How to solve a cubic equation,
// revisited" (http://momentsingraphics.de/?p=105) — upstream SolveCubic.
static inline float3 ltcSolveCubic(float4 coefficient)
{
    const float pi = 3.14159;
    // Normalize the polynomial
    coefficient.xyz /= coefficient.w;
    // Divide middle coefficients by three
    coefficient.yz /= 3.0;

    const float B = coefficient.z;
    const float C = coefficient.y;
    const float D = coefficient.x;

    // Compute the Hessian and the discriminant
    const float3 delta = float3(
        -coefficient.z * coefficient.z + coefficient.y,
        -coefficient.y * coefficient.z + coefficient.x,
        dot(float2(coefficient.z, -coefficient.y), coefficient.xy));

    const float discriminant = dot(float2(4.0 * delta.x, -delta.y), delta.zy);

    float2 xlc, xsc;

    // Algorithm A
    {
        const float C_a = delta.x;
        const float D_a = -2.0 * B * delta.x + delta.y;

        // Take the cubic root of a normalized complex number
        const float theta = atan2(sqrt(discriminant), -D_a) / 3.0;

        const float x_1a = 2.0 * sqrt(-C_a) * cos(theta);
        const float x_3a = 2.0 * sqrt(-C_a) * cos(theta + (2.0 / 3.0) * pi);

        const float xl = ((x_1a + x_3a) > 2.0 * B) ? x_1a : x_3a;
        xlc = float2(xl - B, 1.0);
    }

    // Algorithm D
    {
        const float C_d = delta.z;
        const float D_d = -D * delta.y + 2.0 * C * delta.z;

        // Take the cubic root of a normalized complex number
        const float theta = atan2(D * sqrt(discriminant), -D_d) / 3.0;

        const float x_1d = 2.0 * sqrt(-C_d) * cos(theta);
        const float x_3d = 2.0 * sqrt(-C_d) * cos(theta + (2.0 / 3.0) * pi);

        const float xs = (x_1d + x_3d < 2.0 * C) ? x_1d : x_3d;
        xsc = float2(-D, xs + C);
    }

    const float E = xlc.y * xsc.y;
    const float F = -xlc.x * xsc.y - xlc.y * xsc.x;
    const float G = xlc.x * xsc.x;

    const float2 xmc = float2(C * F - B * G, -B * F + C * E);

    float3 root = float3(xsc.x / xsc.y, xmc.x / xmc.y, xlc.x / xlc.y);

    if (root.x < root.y && root.x < root.z) {
        root.xyz = root.yxz;
    } else if (root.z < root.x && root.z < root.y) {
        root.xyz = root.xzy;
    }
    return root;
}

// Evaluates the LTC integral of a disk light inscribed in the quad p0/p1/p2
// (upstream LTC_EvaluateDisk). LUT2's w channel holds the tabulated
// horizon-clipped sphere scale.
static inline float ltcEvaluateDisk(float3 N, float3 V, float3 P, float3x3 mInv,
    float3 p0, float3 p1, float3 p2, texture2d<float> lut2)
{
    // construct orthonormal basis around N
    const float3 T1 = normalize(V - N * dot(V, N));
    const float3 T2 = cross(N, T1);

    // rotate area light in (T1, T2, N) basis
    const float3x3 R = transpose(float3x3(T1, T2, N));
    const float3 L0 = R * (p0 - P);
    const float3 L1 = R * (p1 - P);
    const float3 L2 = R * (p2 - P);

    // init ellipse
    float3 C  = mInv * (0.5 * (L0 + L2));
    float3 V1 = mInv * (0.5 * (L1 - L2));
    float3 V2 = mInv * (0.5 * (L1 - L0));

    // compute eigenvectors of ellipse
    float a, b;
    const float d11 = dot(V1, V1);
    const float d22 = dot(V2, V2);
    const float d12 = dot(V1, V2);
    if (abs(d12) / sqrt(d11 * d22) > 0.0001) {
        const float tr = d11 + d22;
        float det = -d12 * d12 + d11 * d22;

        // use sqrt matrix to solve for eigenvalues
        det = sqrt(det);
        const float u = 0.5 * sqrt(tr - 2.0 * det);
        const float v = 0.5 * sqrt(tr + 2.0 * det);
        const float e_max = (u + v) * (u + v);
        const float e_min = (u - v) * (u - v);

        float3 V1_, V2_;
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

    float3 V3 = normalize(cross(V1, V2));
    if (dot(C, V3) < 0.0) {
        V3 *= -1.0;
    }

    const float L  = dot(V3, C);
    const float x0 = dot(V1, C) / L;
    const float y0 = dot(V2, C) / L;

    a *= L * L;
    b *= L * L;

    const float c0 = a * b;
    const float c1 = a * b * (1.0 + x0 * x0 + y0 * y0) - a - b;
    const float c2 = 1.0 - a * (1.0 + x0 * x0) - b * (1.0 + y0 * y0);
    const float c3 = 1.0;

    const float3 roots = ltcSolveCubic(float4(c0, c1, c2, c3));
    const float e1 = roots.x;
    const float e2 = roots.y;
    const float e3 = roots.z;

    float3 avgDir = float3(a * x0 / (a - e2), b * y0 / (b - e2), 1.0);

    const float3x3 rotate = float3x3(V1, V2, V3);
    avgDir = normalize(rotate * avgDir);

    const float L1_ = sqrt(-e2 / e3);
    const float L2_ = sqrt(-e2 / e1);

    const float formFactor = max(0.0, L1_ * L2_ * rsqrt((1.0 + L1_ * L1_) * (1.0 + L2_ * L2_)));

    const float LUT_SIZE = 64.0;
    // use tabulated horizon-clipped sphere
    float2 uv = float2(avgDir.z * 0.5 + 0.5, formFactor);
    uv = uv * ((LUT_SIZE - 1.0) / LUT_SIZE) + (0.5 / LUT_SIZE);

    const float scale = lut2.sample(ltcLutSampler, uv, level(0)).w;
    const float result = formFactor * scale;

    // upstream FixNan: the disk evaluator rarely produces NaNs (upstream TODO);
    // zero them before they spread through bloom/DOF blurs.
    return isnan(result) ? 0.0 : result;
}
