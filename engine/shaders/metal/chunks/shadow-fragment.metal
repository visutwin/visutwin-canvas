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
    (void)rd;
    (void)material;
#endif

    // Depth-only pass — depth is written automatically by the rasterizer.
    // Return white for color render target compatibility (VSM path).
    return float4(1.0, 1.0, 1.0, 1.0);
}
