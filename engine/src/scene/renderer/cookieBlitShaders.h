// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The cookie blit's uniform block (the program is engine/shaders/slang/programs/
// cookie-blit.slang): copy a light cookie into its rect of the clustered cookie atlas. A 2D cookie is copied
// as is; a cube cookie is drawn one face at a time, each texel reading the cube along the
// ray of the face camera through it (`invViewProj`), x flipped as every cube read in this
// engine is (the cube convention of the skybox, probes and non-clustered cookies).
//
// Layout, shared with every quad effect: the source is fragment slot 0 (MSL texture(0) /
// GLSL set 1 binding 0) and the uniforms ride the material slot (MSL buffer(3) / GLSL set
// 0 binding 0). The quad's uv has v = 0 at the TOP, so NDC y = 1 - 2v.
//
#pragma once

#include <cstdint>


namespace visutwin::canvas::cookie_shaders
{
    struct alignas(16) CookieBlitUniforms
    {
        float invViewProj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };
    static_assert(sizeof(CookieBlitUniforms) == 64);

}
