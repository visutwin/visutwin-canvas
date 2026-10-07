// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// What the volumetric fog march, combine and local-light programs share.
module vtfog;

/// Standard depth [0, 1] (near 0, far 1) to the positive linear view-space distance.
public float fogLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

/// Interleaved gradient noise, offsetting the march samples to hide banding.
public float fogNoise(float2 fragCoord)
{
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(fragCoord, magic.xy)));
}

/// The normalised Henyey-Greenstein phase function.
public float fogPhase(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (12.56637 * denom * sqrt(max(denom, 1e-6)));
}
