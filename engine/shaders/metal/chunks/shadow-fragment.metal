// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
fragment float4 VT_FRAGMENT_ENTRY(RasterizerData rd [[stage_in]],
                                  constant MaterialData &material [[buffer(3)]],
                                  texture2d<float> baseColorTexture [[texture(0)]],
                                  texture2d<float> opacityTexture [[texture(34)]],
                                  sampler defaultSampler [[sampler(0)]])
{
#if VT_FEATURE_ALPHA_TEST || VT_FEATURE_SHADOW_DITHER
    // Opacity frontend, run before depth is written — upstream's litShadowMain
    // evaluates the material the same way here as in the forward pass, and a
    // shadow computed from anything less is the shadow of a different surface.
    // The alpha has to be the SAME product the forward pass tests: base colour
    // alpha times the base-colour texture's alpha. Testing the factor alone made
    // every masked caster (foliage, fences, cut-out signs) throw the solid shadow
    // of its quad.
    float shadowAlpha = material.baseColor.a;
#if VT_FEATURE_BASE_COLOR_MAP
    // Gated on the feature AND on the runtime size: an unbound Metal texture
    // reports a nonzero width and samples zero, which would discard the whole
    // caster rather than none of it.
    if (baseColorTexture.get_width() > 0 && baseColorTexture.get_height() > 0) {
        float2 uvBase = ((material.flags & (1u << 4)) != 0u) ? rd.uv1 : rd.uv0;
        uvBase = applyUvTransform(uvBase, material.baseColorTransform0, material.baseColorTransform1);
        shadowAlpha *= baseColorTexture.sample(defaultSampler, uvBase).a;
    }
#endif
    // Opacity map, the same product the forward pass tests (flags bit 19, slot 34).
    if ((material.flags & (1u << 19)) != 0u && opacityTexture.get_width() > 0 && opacityTexture.get_height() > 0) {
        float2 uvOpacity = ((material.flags & (1u << 4)) != 0u) ? rd.uv1 : rd.uv0;
        uvOpacity = applyUvTransform(uvOpacity, material.baseColorTransform0, material.baseColorTransform1);
        shadowAlpha *= opacityTexture.sample(defaultSampler, uvOpacity).a;
    }
#else
    // Neither the alpha test nor the dither is in this variant, so the frontend
    // is not compiled at all and the caster writes depth as fast as it can.
    (void)baseColorTexture;
    (void)opacityTexture;
    (void)defaultSampler;
#endif

#if VT_FEATURE_ALPHA_TEST
    if (shadowAlpha < material.alphaCutoff) {
        discard_fragment();
    }
#endif

#if VT_FEATURE_SHADOW_DITHER
    // Shadow-pass opacity dither (upstream opacityShadowDither, flags bits 29-31, kept
    // independent of the forward dither in bits 25-27). A partially-opaque caster discards
    // the same screen-space Bayer pattern here, so it throws a thinned shadow instead of a
    // solid one. The mode is still read at runtime — the feature only says a caster in
    // this pass asked for dithering, which is what gives Vulkan a fragment stage at all.
    {
        const uint shadowDitherMode = (material.flags >> 29) & 0x7u;
        if (shadowDitherMode != 0u) {
            const float ditherStrength = material.dispersionParams.y;
            const float ditherAlpha = ditherStrength >= 0.0 ? ditherStrength : shadowAlpha;
            if (ditherDiscards(shadowDitherMode, rd.position.xy, ditherAlpha)) {
                discard_fragment();
            }
        }
    }
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
    // PCF path: depth-only render target — depth is written automatically by
    // the rasterizer; the color return value is unused (no color RT bound).
    return float4(1.0, 1.0, 1.0, 1.0);
#endif
}
