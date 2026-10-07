// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// The uniform block of Scalable Ambient Obscurance (spiral-tap SAO from the depth
// buffer), engine/shaders/slang/programs/ssao.slang. Texture: quad slot 0 = scene depth.
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::ssao_shaders
{
    /**
     * Must match the SsaoParams/SsaoUniforms block in both shaders below. Every
     * vec2 is preceded by an explicit pad so it lands 8-byte aligned, which is
     * what MSL and std140 both require — that keeps one C++ struct valid for both.
     */
    struct alignas(16) SsaoUniforms
    {
        float aspect = 1.0f;                      // offset  0
        float _pad0 = 0.0f;                       // offset  4
        float invResolution[2] = {0.0f, 0.0f};    // offset  8
        float sampleCount[2] = {12.0f, 1.0f};     // offset 16  x = count, y = 1/count
        float spiralTurns = 10.0f;                // offset 24
        float _pad1 = 0.0f;                       // offset 28
        float angleIncCosSin[2] = {0.0f, 0.0f};   // offset 32
        float maxLevel = 0.0f;                    // offset 40
        float invRadiusSquared = 0.0f;            // offset 44
        float minHorizonAngleSineSquared = 0.0f;  // offset 48
        float bias = 0.001f;                      // offset 52
        float peak2 = 0.0f;                       // offset 56
        float intensity = 0.0f;                   // offset 60
        float power = 6.0f;                       // offset 64
        float projectionScaleRadius = 0.0f;       // offset 68
        float randomize = 0.0f;                   // offset 72
        float cameraNear = 0.1f;                  // offset 76
        float cameraFar = 1000.0f;                // offset 80
    };
    static_assert(sizeof(SsaoUniforms) == 96);

}
