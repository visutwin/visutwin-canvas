// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.10.2026
//
// A reversible tone mapping (Karis): an HDR colour compressed into [0, 1) by its largest
// component. Filtering or blending colours compressed by it keeps one bright sample from
// dominating the result, as it does in linear HDR, where the display tone mapping then shows
// the average of a bright and a dark sample nearly as bright as the bright one. Expanding the
// result restores the HDR range. Apply it to the EXPOSED colour, the one the display tone
// mapping is applied to. The TAA resolve and compose's sharpening share it.
module vttonemapreversible;

public float3 reversibleTonemap(float3 c)
{
    return c / (1.0 + max(c.r, max(c.g, c.b)));
}

public float3 reversibleTonemapInverse(float3 c)
{
    return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4);
}
