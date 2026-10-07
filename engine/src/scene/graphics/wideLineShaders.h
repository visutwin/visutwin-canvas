// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2026
//
// The wide-line draw's template size. The shader is the Slang program
// engine/shaders/slang/programs/wide-line.slang: one instance per SEGMENT, its template
// geometry generated from the vertex id (a quad for the body, two discs for round caps
// and joins, two triangles for bevel joins), so every instance draws the same count.
#pragma once

namespace visutwin::canvas::wideline
{
    // Template layout, which the shader's templateVertex follows:
    //   0 ..  5   quad body        (kind 0)
    //   6 .. 53   start disc       (kind 1)  16 triangles
    //  54 ..101   end disc         (kind 2)  16 triangles
    // 102 ..107   bevel triangles  (kinds 3/4/5)
    inline constexpr int kRoundSegments = 16;
    inline constexpr int kTemplateVertices = 6 + 2 * (kRoundSegments * 3) + 6;
}
