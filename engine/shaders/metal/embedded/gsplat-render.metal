#include <metal_stdlib>
using namespace metal;

struct GpuSplat {
    packed_float3 center;
    uint color;             // RGBA8: SH0 color + opacity
    packed_float3 covA;     // Sigma00, Sigma01, Sigma02
    packed_float3 covB;     // Sigma11, Sigma12, Sigma22
};

struct GSplatParams {
    float4x4 modelView;
    float4x4 projection;    // GL-style clip (z in [-w,w]); remapped below
    float4 viewport;        // width, height, 1/width, 1/height
    uint splatCount;
    uint shBands;           // 0 = SH0 only; 1-3 = view-dependent SH color
    uint pad1; uint pad2;
    float4 fogColor;        // linear rgb
    float4 fogParams;       // start, end, density, type (0 = none, 1 linear, 2 exp, 3 exp2)
    float4 output;          // exposure, tone mapping mode, linear HDR target, unused
};

// toneMap(color, exposure, mode) is the forward pass's own, from the common-tonemap
// chunk, which GSplatResource splices in after the metal_stdlib prologue below.

// Upstream gsplatOutput's prepareOutputFromGamma, with the port's targets: a camera
// frame's scene pass is linear HDR with tone mapping deferred to compose (upstream's
// GAMMA_NONE + TONEMAP_NONE there), anything else is a gamma target that the forward
// tail tonemaps and encodes. `depth` is the view depth, as the forward fog uses.
static inline float3 gsplatPrepareOutput(float3 gammaColor, float depth, constant GSplatParams& params)
{
    const bool linearTarget = params.output.z > 0.5;
    const uint fogType = uint(params.fogParams.w + 0.5);
    const bool fog = fogType != 0u;
    // TONEMAP_NONE (6) skips tone mapping, and a linear target leaves it to compose.
    const bool tonemap = !linearTarget && params.output.y < 5.5;

    float3 color = gammaColor;
    if (tonemap || linearTarget || fog) {
        color = pow(max(color, float3(0.0)), float3(2.2));          // upstream decodeGamma
    }
    if (fog) {
        const float density = max(params.fogParams.z, 0.0);
        float fogFactor;
        if (fogType == 1u) {
            const float fogEnd = max(params.fogParams.y, params.fogParams.x + 1e-3);
            fogFactor = (fogEnd - depth) / (fogEnd - params.fogParams.x);
        } else if (fogType == 2u) {
            fogFactor = exp(-depth * density);
        } else {
            const float d = depth * density;
            fogFactor = exp(-d * d);
        }
        color = mix(params.fogColor.xyz, color, saturate(fogFactor));
    }
    if (tonemap) {
        color = toneMap(color, max(params.output.x, 0.0), params.output.y);
    }
    if (tonemap || (!linearTarget && fog)) {
        color = pow(max(color, float3(0.0)) + 0.0000001, float3(1.0 / 2.2)); // upstream gammaCorrectOutput
    }
    return color;
}

struct GSplatVaryings {
    float4 position [[position]];
    float2 uv;
    half4 color;
};

// Spherical-harmonics evaluation (upstream gsplatEvalSH.js). sh is the per-splat
// coefficient buffer; base = splatIndex*45; coefficients are coefficient-major
// interleaved (sh[k] = rgb of coefficient k). Returns the view-dependent color
// added to the DC term in display/gamma space (before the sRGB→linear decode).
static inline float3 gsplatEvalSH(constant float* sh, uint base, uint bands, float3 dir)
{
    const float x = dir.x, y = dir.y, z = dir.z;
#define SHV(k) float3(sh[base + (k) * 3u + 0u], sh[base + (k) * 3u + 1u], sh[base + (k) * 3u + 2u])

    // 1st degree
    const float SH_C1 = 0.4886025119029199;
    float3 result = SH_C1 * (-SHV(0) * y + SHV(1) * z - SHV(2) * x);

    if (bands > 1u) {
        // 2nd degree
        const float xx = x * x, yy = y * y, zz = z * z;
        const float xy = x * y, yz = y * z, xz = x * z;
        const float C2_0 = 1.0925484305920792, C2_1 = -1.0925484305920792,
                    C2_2 = 0.31539156525252005, C2_3 = -1.0925484305920792,
                    C2_4 = 0.5462742152960396;
        result += SHV(3) * (C2_0 * xy) +
                  SHV(4) * (C2_1 * yz) +
                  SHV(5) * (C2_2 * (2.0 * zz - xx - yy)) +
                  SHV(6) * (C2_3 * xz) +
                  SHV(7) * (C2_4 * (xx - yy));
    }

    if (bands > 2u) {
        // 3rd degree
        const float xx = x * x, yy = y * y, zz = z * z, xy = x * y;
        const float C3_0 = -0.5900435899266435, C3_1 = 2.890611442640554,
                    C3_2 = -0.4570457994644658, C3_3 = 0.3731763325901154,
                    C3_4 = -0.4570457994644658, C3_5 = 1.445305721320277,
                    C3_6 = -0.5900435899266435;
        result += SHV(8)  * (C3_0 * y * (3.0 * xx - yy)) +
                  SHV(9)  * (C3_1 * xy * z) +
                  SHV(10) * (C3_2 * y * (4.0 * zz - xx - yy)) +
                  SHV(11) * (C3_3 * z * (2.0 * zz - 3.0 * xx - 3.0 * yy)) +
                  SHV(12) * (C3_4 * x * (4.0 * zz - xx - yy)) +
                  SHV(13) * (C3_5 * z * (xx - yy)) +
                  SHV(14) * (C3_6 * x * (xx - 3.0 * yy));
    }
#undef SHV
    return result;
}

vertex GSplatVaryings gsplatVS(uint vid [[vertex_id]],
                               uint iid [[instance_id]],
                               constant GpuSplat* splats [[buffer(7)]],
                               constant uint* order [[buffer(8)]],
                               constant GSplatParams& params [[buffer(11)]],
                               constant float* shCoeffs [[buffer(12)]])
{
    GSplatVaryings out;
    out.position = float4(0.0, 0.0, 2.0, 1.0);  // default: clipped
    out.uv = float2(0.0);
    out.color = half4(0.0h);

    const float2 cornerUV[4] = { float2(-1.0, -1.0), float2(1.0, -1.0),
                                 float2(-1.0, 1.0), float2(1.0, 1.0) };

    const uint splatIndex = order[iid];
    if (splatIndex >= params.splatCount) {
        return out;
    }
    const GpuSplat s = splats[splatIndex];

    const float4 view = params.modelView * float4(float3(s.center), 1.0);
    float4 clip = params.projection * view;
    if (clip.w <= 0.0) {
        return out;
    }

    // 3D covariance in splat model space.
    const float3 covA = float3(s.covA);
    const float3 covB = float3(s.covB);
    const float3x3 Vrk = float3x3(float3(covA.x, covA.y, covA.z),
                                  float3(covA.y, covB.x, covB.y),
                                  float3(covA.z, covB.y, covB.z));

    // Perspective Jacobian at the splat center (upstream gsplatCorner.js).
    const float focal = params.viewport.x * params.projection[0][0];
    const float3 v = view.xyz;
    const float J1 = focal / v.z;
    const float2 J2 = -J1 / v.z * v.xy;
    const float3x3 J = float3x3(float3(J1, 0.0, J2.x),
                                float3(0.0, J1, J2.y),
                                float3(0.0, 0.0, 0.0));

    const float3x3 W = transpose(float3x3(params.modelView[0].xyz,
                                          params.modelView[1].xyz,
                                          params.modelView[2].xyz));
    const float3x3 T = W * J;
    const float3x3 cov = transpose(T) * Vrk * T;

    // 2D covariance eigen decomposition (+0.3 px low-pass dilation).
    const float diagonal1 = cov[0][0] + 0.3;
    const float offDiagonal = cov[0][1];
    const float diagonal2 = cov[1][1] + 0.3;
    const float mid = 0.5 * (diagonal1 + diagonal2);
    const float radius = length(float2((diagonal1 - diagonal2) * 0.5, offDiagonal));
    const float lambda1 = mid + radius;
    const float lambda2 = max(mid - radius, 0.1);

    const float vmin = min(1024.0, min(params.viewport.x, params.viewport.y));
    const float l1 = 2.0 * min(sqrt(2.0 * lambda1), vmin);
    const float l2 = 2.0 * min(sqrt(2.0 * lambda2), vmin);

    // Cull sub-pixel gaussians.
    if (max(l1, l2) < 0.5) {
        return out;
    }

    // Cull against the frustum x/y planes.
    const float2 c = clip.w * params.viewport.zw;
    if (any(abs(clip.xy) - float2(max(l1, l2)) * c > clip.ww)) {
        return out;
    }

    const float2 diagonalVector = normalize(float2(offDiagonal, lambda1 - diagonal1));
    const float2 v1 = l1 * diagonalVector;
    const float2 v2 = l2 * float2(diagonalVector.y, -diagonalVector.x);

    const float2 uv = cornerUV[vid];
    clip.xy += (uv.x * v1 + uv.y * v2) * c;

    // DEVIATION: OpenGL NDC z range is [-1,1]; Metal requires [0,1].
    clip.z = 0.5 * (clip.z + clip.w);

    // Keep the splat off the near and far planes (upstream gsplatCenter.js, which
    // clamps to [0, |w|] on the [0,1] depth convention this remap produces).
    //
    // A splat is a screen-space footprint built around ONE projected centre, so the
    // whole quad shares that centre's depth. Without this, a centre that crosses the
    // near plane clips the entire splat away even while its footprint still covers
    // visible pixels — the surface nearest the camera pops out whole as you walk into
    // a cloud, which is exactly where it is largest and most missed. The far plane
    // does the same on the way out.
    //
    // The cost is that a clamped splat depth-tests as though it sat ON the plane. That
    // is upstream's trade and it is the cheaper error: splats blend back-to-front from
    // the order buffer, so their mutual order comes from the sort rather than from
    // depth, and only their occlusion against opaque geometry is affected.
    //
    // clip.w > 0 here — the behind-camera case returned above — so no abs() is needed.
    clip.z = clamp(clip.z, 0.0, clip.w);

    const half4 color = unpack_unorm4x8_to_half(s.color);
    float3 displayColor = float3(color.rgb);

    // View-dependent spherical harmonics (added in display/gamma space before the
    // sRGB→linear decode, matching upstream). dir = model-space view direction.
    if (params.shBands > 0u) {
        const float3 viewPos = view.xyz;
        const float3x3 mv3 = float3x3(params.modelView[0].xyz,
                                      params.modelView[1].xyz,
                                      params.modelView[2].xyz);
        const float3 dir = normalize(transpose(mv3) * viewPos);
        displayColor += gsplatEvalSH(shCoeffs, splatIndex * 45u, params.shBands, dir);
    }

    out.position = clip;
    out.uv = uv;
    // The colour is gamma space; the output stage makes it what the target wants.
    // clip.w is the view depth (positive here), the same depth the forward fog uses.
    out.color = half4(half3(gsplatPrepareOutput(max(displayColor, float3(0.0)), clip.w, params)), color.a);
    return out;
}

fragment half4 gsplatFS(GSplatVaryings in [[stage_in]])
{
    const float A = dot(in.uv, in.uv);
    if (A > 1.0) {
        discard_fragment();
    }
    // Normalized exponential falloff (upstream normExp): 1 at center, 0 at edge.
    const float EXP4 = 0.018315638889f;
    const float alpha = ((exp(-4.0f * A) - EXP4) / (1.0f - EXP4)) * float(in.color.a);
    return half4(half3(in.color.rgb) * half(alpha), half(alpha));
}
