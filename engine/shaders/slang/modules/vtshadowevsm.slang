// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.10.2026
//
// The exponent of the depth warp of the VSM shadows (EVSM 16F), shared by every shader that
// writes the moments (the mesh and splat casters) and every one that tests against them (the
// forward shadow lookups, the volumetric fog march). The two sides must agree.
module vtshadowevsm;

// the largest exponent keeping the stored square of the warped depth, exp(2 * exponent),
// within the range of a half float
public static const float kVsmExponent16F = 5.54;
