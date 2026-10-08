// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// ── LTC area lights ──
// Real-Time Polygonal-Light Shading with Linearly Transformed Cosines
// (Heitz, Dupuy, Hill, Neubelt). Rect, disk and sphere shapes.

static const float LTC_LUT_SIZE = 64.0;

// LUT coordinate. The LUT is indexed by (1 - gloss) squared, gloss being
// 1 - perceptualRoughness, so this is the same value expressed directly.
float2 ltcUv(float3 N, float3 V, float perceptualRoughness) {
    float lutRoughness = max(perceptualRoughness * perceptualRoughness, 0.001);
    float dotNV = clamp(dot(N, V), 0.0, 1.0);
    float2 uv = float2(lutRoughness, sqrt(1.0 - dotNV));
    return uv * ((LTC_LUT_SIZE - 1.0) / LTC_LUT_SIZE) + (0.5 / LTC_LUT_SIZE);
}

// Range window only: a non-punctual light gets its
// physical distance falloff from the LTC form factor, so only the artist-set
// range is applied here. Range 0 is clamped,
// so it extinguishes the light rather than lifting the range limit (the renderer
// never uploads less than 1e-4 anyway).
float ltcFalloffWindow(float range, float3 toLight) {
    float sqrDist = dot(toLight, toLight);
    float invRadius = 1.0 / max(range, 1e-4);
    float t = sqrDist * invRadius * invRadius;
    float w = clamp(1.0 - t * t, 0.0, 1.0);
    return w * w;
}

// Form factor of a horizon-clipped rectangle ("Real-Time Area Lighting: a
// Journey from Research to Production", p.102).
float ltcClippedSphereFormFactor(float3 f) {
    float l = length(f);
    return max((l * l + f.z) / (l + 1.0), 0.0);
}

float3 ltcEdgeVectorFormFactor(float3 v1, float3 v2) {
    float x = dot(v1, v2);
    float y = abs(x);
    // rational polynomial approximation to theta / sin(theta) / 2PI
    float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float b = 3.4175940 + (4.1616724 + y) * y;
    float v = a / b;
    float thetaSintheta = (x > 0.0)
        ? v
        : 0.5 * rsqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * thetaSintheta;
}

// LTC integral of a rect light (corners p0..p3, ccw) over the cosine
// distribution transformed by mInv, as seen from surface point P.
float ltcEvaluateRect(float3 N, float3 V, float3 P, float3x3 mInv,
                      float3 p0, float3 p1, float3 p2, float3 p3) {
    float3 v1 = p1 - p0;
    float3 v2 = p3 - p0;
    float3 lightNormal = cross(v1, v2);
    float factor = sign(-dot(lightNormal, P - p0));

    // orthonormal basis around N
    float3 T1 = normalize(V - N * dot(V, N));
    float3 T2 = factor * cross(N, T1); // negated from the paper due to handedness

    float3x3 mat = mul(transpose(float3x3(T1, T2, N)), mInv);

    // transform rect corners and project onto sphere
    float3 c0 = normalize(mul((p0 - P), mat));
    float3 c1 = normalize(mul((p1 - P), mat));
    float3 c2 = normalize(mul((p2 - P), mat));
    float3 c3 = normalize(mul((p3 - P), mat));

    float3 vectorFormFactor = float3(0.0);
    vectorFormFactor += ltcEdgeVectorFormFactor(c0, c1);
    vectorFormFactor += ltcEdgeVectorFormFactor(c1, c2);
    vectorFormFactor += ltcEdgeVectorFormFactor(c2, c3);
    vectorFormFactor += ltcEdgeVectorFormFactor(c3, c0);

    return ltcClippedSphereFormFactor(vectorFormFactor);
}

// Extended cubic solver from "How to solve a cubic equation, revisited"
// (http://momentsingraphics.de/?p=105).
float3 ltcSolveCubic(float4 coefficient) {
    const float pi = 3.14159;
    coefficient.xyz /= coefficient.w;
    coefficient.yz /= 3.0;

    float B = coefficient.z;
    float C = coefficient.y;
    float D = coefficient.x;

    float3 delta = float3(
        -coefficient.z * coefficient.z + coefficient.y,
        -coefficient.y * coefficient.z + coefficient.x,
        dot(float2(coefficient.z, -coefficient.y), coefficient.xy));

    float discriminant = dot(float2(4.0 * delta.x, -delta.y), delta.zy);

    float2 xlc, xsc;

    // Algorithm A
    {
        float C_a = delta.x;
        float D_a = -2.0 * B * delta.x + delta.y;

        float theta = atan2(sqrt(discriminant), -D_a) / 3.0;

        float x_1a = 2.0 * sqrt(-C_a) * cos(theta);
        float x_3a = 2.0 * sqrt(-C_a) * cos(theta + (2.0 / 3.0) * pi);

        float xl = ((x_1a + x_3a) > 2.0 * B) ? x_1a : x_3a;
        xlc = float2(xl - B, 1.0);
    }

    // Algorithm D
    {
        float C_d = delta.z;
        float D_d = -D * delta.y + 2.0 * C * delta.z;

        float theta = atan2(D * sqrt(discriminant), -D_d) / 3.0;

        float x_1d = 2.0 * sqrt(-C_d) * cos(theta);
        float x_3d = 2.0 * sqrt(-C_d) * cos(theta + (2.0 / 3.0) * pi);

        float xs = (x_1d + x_3d < 2.0 * C) ? x_1d : x_3d;
        xsc = float2(-D, xs + C);
    }

    float E = xlc.y * xsc.y;
    float F = -xlc.x * xsc.y - xlc.y * xsc.x;
    float G = xlc.x * xsc.x;

    float2 xmc = float2(C * F - B * G, -B * F + C * E);

    float3 root = float3(xsc.x / xsc.y, xmc.x / xmc.y, xlc.x / xlc.y);

    if (root.x < root.y && root.x < root.z) {
        root.xyz = root.yxz;
    } else if (root.z < root.x && root.z < root.y) {
        root.xyz = root.xzy;
    }
    return root;
}

// LTC integral of a disk light inscribed in the quad p0/p1/p2.
// LUT2's w channel holds the horizon-clipped sphere scale.
float ltcEvaluateDisk(float3 N, float3 V, float3 P, float3x3 mInv,
                      float3 p0, float3 p1, float3 p2) {
    // orthonormal basis around N
    float3 T1 = normalize(V - N * dot(V, N));
    float3 T2 = cross(N, T1);

    // rotate area light into the (T1, T2, N) basis
    float3x3 R = transpose(float3x3(T1, T2, N));
    float3 L0 = mul((p0 - P), R);
    float3 L1 = mul((p1 - P), R);
    float3 L2 = mul((p2 - P), R);

    // init ellipse
    float3 C  = mul((0.5 * (L0 + L2)), mInv);
    float3 V1 = mul((0.5 * (L1 - L2)), mInv);
    float3 V2 = mul((0.5 * (L1 - L0)), mInv);

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

    float L  = dot(V3, C);
    float x0 = dot(V1, C) / L;
    float y0 = dot(V2, C) / L;

    a *= L * L;
    b *= L * L;

    float c0 = a * b;
    float c1 = a * b * (1.0 + x0 * x0 + y0 * y0) - a - b;
    float c2 = 1.0 - a * (1.0 + x0 * x0) - b * (1.0 + y0 * y0);
    float c3 = 1.0;

    float3 roots = ltcSolveCubic(float4(c0, c1, c2, c3));
    float e1 = roots.x;
    float e2 = roots.y;
    float e3 = roots.z;

    float3 avgDir = float3(a * x0 / (a - e2), b * y0 / (b - e2), 1.0);

    float3x3 rotate = float3x3(V1, V2, V3);
    avgDir = normalize(mul(avgDir, rotate));

    float L1_ = sqrt(-e2 / e3);
    float L2_ = sqrt(-e2 / e1);

    float formFactor = max(0.0,
        L1_ * L2_ * rsqrt((1.0 + L1_ * L1_) * (1.0 + L2_ * L2_)));

    // tabulated horizon-clipped sphere
    float2 uv = float2(avgDir.z * 0.5 + 0.5, formFactor);
    uv = uv * ((LTC_LUT_SIZE - 1.0) / LTC_LUT_SIZE) + (0.5 / LTC_LUT_SIZE);

    float scale = areaLightLut2.SampleLevel(linearClampSampler, uv, 0.0).w;
    float result = formFactor * scale;

    // FixNan: the disk evaluator rarely produces NaNs; zero them
    // before they spread through bloom/DOF blurs.
    return isnan(result) ? 0.0 : result;
}


// ── A light with a non-punctual shape (LightShape 1 rect, 2 disk, 3 sphere), as both
// the main light loop and the cluster loop evaluate it. The corners come back through
// p0..p3.
float ltcAreaLight(uint shape, float3 lightPos, float3 halfWidth, float3 halfHeight, float3 N,
                   float3 cameraPosition, out float3 p0, out float3 p1, out float3 p2, out float3 p3) {
    float sphereRadius = max(length(halfWidth), length(halfHeight));
    if (shape == 3u) {
        // Billboard to the reflection vector so the disk integral can take it.
        float3 f = reflect(normalize(lightPos - cameraPosition), N);
        float3 w = normalize(cross(f, halfHeight));
        float3 h = normalize(cross(f, w));
        halfWidth = w * sphereRadius;
        halfHeight = h * sphereRadius;
    }
    p0 = lightPos + halfWidth - halfHeight;
    p1 = lightPos - halfWidth - halfHeight;
    p2 = lightPos - halfWidth + halfHeight;
    p3 = lightPos + halfWidth + halfHeight;
    return sphereRadius;
}

// Diffuse form factor of a LOCAL area light, with the 16 that the punctual
// inverse-square falloff carries.
float ltcAreaDiffuse(uint shape, float sphereRadius, float3 lightPos, float3 N, float3 V, float3 P,
                     float3 p0, float3 p1, float3 p2, float3 p3) {
    if (shape == 2u) {
        return ltcEvaluateDisk(N, V, P, float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0), p0, p1, p2) * 16.0;
    }
    if (shape == 3u) {
        // Punctual Lambert with a radius-based falloff.
        float3 toLight = lightPos - P;
        float falloff = sphereRadius / (dot(toLight, toLight) + sphereRadius);
        return max(dot(N, normalize(toLight)), 0.0) * falloff * 16.0;
    }
    return ltcEvaluateRect(N, V, P, float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0), p0, p1, p2, p3) * 16.0;
}

// Fresnel magnitude and geometric attenuation from LUT2.
float3 ltcSpecularFresnel(float3 N, float3 V, float perceptualRoughness, float3 F0) {
    float4 t2 = areaLightLut2.SampleLevel(linearClampSampler, ltcUv(N, V, perceptualRoughness), 0.0);
    return F0 * t2.x + (float3(1.0) - F0) * t2.y;
}

// Specular LTC integral with the inverse transform from LUT1 (a sphere integrates
// its billboarded quad as a disk).
float ltcAreaSpecular(uint shape, float3 N, float3 V, float3 P, float perceptualRoughness,
                      float3 p0, float3 p1, float3 p2, float3 p3) {
    float4 t1 = areaLightLut1.SampleLevel(linearClampSampler, ltcUv(N, V, perceptualRoughness), 0.0);
    float3x3 mInv = float3x3(
        float3(t1.x, 0.0, t1.y),
        float3(0.0, 1.0, 0.0),
        float3(t1.z, 0.0, t1.w));
    return (shape != 1u)
        ? ltcEvaluateDisk(N, V, P, mInv, p0, p1, p2)
        : ltcEvaluateRect(N, V, P, mInv, p0, p1, p2, p3);
}
