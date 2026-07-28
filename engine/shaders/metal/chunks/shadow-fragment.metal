// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
fragment float4 VT_FRAGMENT_ENTRY(RasterizerData rd [[stage_in]],
                                  constant MaterialData &material [[buffer(3)]])
{
#if VT_FEATURE_ALPHA_TEST
    // Alpha test for masked materials (e.g. foliage).
    // Sample diffuse texture if available, otherwise use base color alpha.
    float alpha = material.baseColor.a;
    if (alpha < material.alphaCutoff) {
        discard_fragment();
    }
#else
    (void)material;
#endif

#if VT_FEATURE_VSM_SHADOWS
    // EVSM_16F output: write (exp(c·z), exp(c·z)², 1, 1) into RGBA16F color RT.
    // Receiver-depth at sample time is also exponentially warped, so the
    // stored moments (after gaussian blur) feed Chebyshev's inequality
    // directly. The .z = 1 marks the pixel as "rendered" for the
    // (1 - moments.z) fallback in calculateEVSM(); cleared pixels are (0,0,0,0)
    // and synthesize "fully lit" at sample time.
    const float ndcZ = rd.position.z;            // Metal: depth ∈ [0, 1]

    // Rasterization of degenerate triangles, which animated (skinned/morphed) meshes can
    // generate, can supply depth outside of the [0, 1] range or even NaN.  The exponential
    // warp below turns those into huge values, which the VSM blur then spreads over a large
    // area, generating visible artifacts.  The depth range is not clipped for the other
    // shadow types, as they either store depth in the depth buffer, which clamps it, or the
    // error stays confined to individual texels and so is not noticeable.
    //
    // DEVIATION: upstream writes this as `if (!(depth >= 0.0 && depth <= 1.0)) discard;`,
    // relying on every comparison against NaN being false.  MetalShader compiles with
    // setFastMathEnabled(true), which tags the compares `fast` (implying nnan) and so lets
    // the compiler assume the NaN case cannot happen — the NaN half of that guard is not
    // guaranteed to survive optimization.  The bit-pattern test below is integer-only
    // (exponent all ones + non-zero mantissa), which fast math cannot elide.
    const uint zBits = as_type<uint>(ndcZ);
    const bool zIsNan = (zBits & 0x7F800000u) == 0x7F800000u && (zBits & 0x007FFFFFu) != 0u;
    if (zIsNan || ndcZ < 0.0 || ndcZ > 1.0) {
        discard_fragment();
    }

    const float warpedZ = exp(VSM_EXPONENT * (2.0 * ndcZ - 1.0));
    return float4(warpedZ, warpedZ * warpedZ, 1.0, 1.0);
#else
    (void)rd;
    // PCF path: depth-only render target — depth is written automatically by
    // the rasterizer; the color return value is unused (no color RT bound).
    return float4(1.0, 1.0, 1.0, 1.0);
#endif
}
