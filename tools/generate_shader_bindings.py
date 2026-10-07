#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2025-2026 Arnis Lektauers
#
# Created by Arnis Lektauers on 07.10.2026
#
"""Writes bindings.slang from the binding table (engine/src/platform/graphics/shaderBindings.h).

Each row becomes one declaration macro carrying BOTH annotations a Slang module needs:
`register(tN)` / `register(sN)`, which decides the Metal slot (Metal ignores
`vk::binding`, and a sampler index above 15 fails Apple's compiler), and
`[[vk::binding(binding, set)]]`, which decides the Vulkan and WGSL one. A Slang module
writes `VT_DECLARE_baseColorMap` instead of spelling either number. Combined-sampler rows
also get a companion sampler declaration on the Metal sampler slot their map carries
(1 + the row's index among the maps that own a sampler, as MetalTextureBinder binds them).

Nothing consumes this file yet: it is the single source the Slang modules will declare
their bindings from (phase 2 of the single-source shader migration).
"""
from __future__ import annotations

import argparse
from pathlib import Path

from shader_bindings import MATERIAL_SET, SCENE_SET, BindingRow, read_bindings

SLANG_TYPE = {
    "Tex2D": "Texture2D",
    "TexCube": "TextureCube",
    "Tex2DArray": "Texture2DArray",
}


def declaration(row: BindingRow, set_index: int, sampler_slot: int | None) -> list[str]:
    """One VT_DECLARE_<name> macro. A map that owns a sampler is a COMBINED declaration
    (`Sampler2D name : register(tN) : register(sK)`): one combined image sampler on
    Vulkan, the texture slot plus the Metal sampler slot its map carries on Metal."""
    where = (f"Metal {'sampler' if row.type == 'Sampler' else 'texture'} "
             f"{row.metal_slot - 100 if row.metal_slot >= 100 else row.metal_slot}"
             if row.metal_slot >= 0 else "no Metal slot (a constexpr sampler)")
    if row.metal_slot >= 100:
        where += " (vertex stage)"
    where += (f", Vulkan set {set_index} binding {row.vk_binding}" if row.vk_binding >= 0 else ", Metal only")
    lines = [f"// {row.name}: {where}"]
    vk = f"[[vk::binding({row.vk_binding}, {set_index})]] " if row.vk_binding >= 0 else ""
    if row.type == "Sampler":
        reg = f" : register(s{row.metal_slot})" if row.metal_slot >= 0 else ""
        lines.append(f"#define VT_DECLARE_{row.name} {vk}SamplerState {row.name}{reg};")
        return lines
    metal_slot = row.metal_slot - 100 if row.metal_slot >= 100 else row.metal_slot
    if sampler_slot is not None:
        lines.append(f"#define VT_DECLARE_{row.name} {vk}Sampler2D {row.name} : register(t{metal_slot}) : register(s{sampler_slot});")
    else:
        lines.append(f"#define VT_DECLARE_{row.name} {vk}{SLANG_TYPE[row.type]} {row.name} : register(t{metal_slot});")
    return lines


# Metal sampler slots of the quad inputs. Metal gives every combined sampler of a function its
# own [[sampler(n)]] argument and rejects two at one index, so each input owns a slot:
# input n reads the post sampler at QUAD_LINEAR_SAMPLER_BASE + n, a point or depth input the
# nearest sampler at QUAD_POINT_SAMPLER_BASE + n. Metal has 16 sampler slots per stage, so a
# point input exists for n < QUAD_POINT_INPUTS only. MetalGraphicsDevice::bindDrawSampler
# binds both ranges for every quad draw; keep the three numbers in step with it.
QUAD_LINEAR_SAMPLER_BASE = 3
QUAD_POINT_SAMPLER_BASE = 11
QUAD_POINT_INPUTS = 5


def quad_inputs(material: list[BindingRow]) -> list[str]:
    """VT_DECLARE_QUAD_INPUTn(name): a quad pass's input n. On Metal input n is texture n
    with a sampler slot of its own (see QUAD_LINEAR_SAMPLER_BASE); on Vulkan it is the n-th
    material binding, combined where the row is, else a separate image through the shared
    material sampler (which VT_DECLARE_QUAD_SAMPLER declares)."""
    lines = ["", "// ---- Quad pass inputs: slot n -> Metal texture n, Vulkan material row n ----",
             "// The uniform block rides the per-draw material slot: Metal buffer 3, Vulkan set 0 binding 0.",
             f"#define VT_DECLARE_QUAD_UNIFORMS(T, name) [[vk::binding(0, 0)]] ConstantBuffer<T> name : register(b3);",
             "// The same slot carries a draw's MATERIAL block (a custom shader's own uniforms).",
             f"#define VT_DECLARE_MATERIAL_UNIFORMS(T, name) [[vk::binding(0, 0)]] ConstantBuffer<T> name : register(b3);",
             "// Metal binds these sampler states for every quad draw (MetalGraphicsDevice::bindDrawSampler):",
             "// 0 the post sampler (linear, clamp, linear mips), 1 nearest-clamp without mips, 2 a linear",
             "// less-equal comparison sampler (clamp), then the post sampler again at",
             f"// {QUAD_LINEAR_SAMPLER_BASE}..{QUAD_LINEAR_SAMPLER_BASE + 7} (input n's linear slot) and the nearest one at "
             f"{QUAD_POINT_SAMPLER_BASE}..{QUAD_POINT_SAMPLER_BASE + QUAD_POINT_INPUTS - 1}",
             "// (input n's point slot: depth taps must be point sampled, and Vulkan binds its nearest",
             "// sampler for every depth texture of a quad pass)."]
    sampler_row = next(r for r in material if r.kind == "Sampler")
    lines.append(f"#define VT_DECLARE_QUAD_SAMPLER [[vk::binding({sampler_row.vk_binding}, {MATERIAL_SET})]] "
                 f"SamplerState quadSampler : register(s0);")
    lines.append("// The Metal comparison sampler (slot 2). Read only by VT_SHADOW_COMPARE on Metal, so the")
    lines.append("// SPIR-V never references it and the binding it names is never laid out.")
    lines.append(f"#define VT_DECLARE_QUAD_COMPARE_SAMPLER [[vk::binding(63, {MATERIAL_SET})]] "
                 f"SamplerComparisonState quadCompareSampler : register(s2);")
    metal_only = []   # declarations that differ per target, emitted under VT_TARGET_METAL
    other = []
    for n, row in enumerate(material[:8]):
        vk = f"[[vk::binding({row.vk_binding}, {MATERIAL_SET})]] "
        linear = QUAD_LINEAR_SAMPLER_BASE + n
        point = QUAD_POINT_SAMPLER_BASE + n
        if row.kind == "CombinedSampler":
            lines.append(f"#define VT_DECLARE_QUAD_INPUT{n}(name) {vk}Sampler2D name : register(t{n}) : register(s{linear});")
            lines.append(f"#define VT_DECLARE_QUAD_CUBE_INPUT{n}(name) {vk}SamplerCube name : register(t{n}) : register(s{linear});")
            if n < QUAD_POINT_INPUTS:
                lines.append(f"#define VT_DECLARE_QUAD_DEPTH_INPUT{n}(name) {vk}Sampler2D<float> name : register(t{n}) : register(s{point});")
                lines.append(f"#define VT_DECLARE_QUAD_POINT_INPUT{n}(name) {vk}Sampler2D name : register(t{n}) : register(s{point});")
                # A depth map for comparison taps, read with VT_SHADOW_COMPARE: on Metal a DEPTH
                # texture through the comparison sampler (the hardware compare, bilinear), on Vulkan
                # the point-sampled combined sampler compared by hand (no comparison sampler fits
                # MoltenVK's 16-sampler budget). It must be DepthTexture2D on Metal: a Texture2D
                # is declared texture2d and pointer-cast to depth2d for sample_compare, which
                # reads as fully shadowed (a local light's fog vanished).
                metal_only.append(f"#define VT_DECLARE_QUAD_SHADOW_INPUT{n}(name) {vk}DepthTexture2D name : register(t{n});")
                other.append(f"#define VT_DECLARE_QUAD_SHADOW_INPUT{n}(name) {vk}Sampler2D<float> name : register(t{n}) : register(s{point});")
            # An input read only by texel (Load): a plain image on Metal, where a Load through a
            # combined sampler does not compile, the combined sampler on Vulkan.
            metal_only.append(f"#define VT_DECLARE_QUAD_TEXEL_INPUT{n}(name) {vk}Texture2D name : register(t{n});")
            other.append(f"#define VT_DECLARE_QUAD_TEXEL_INPUT{n}(name) {vk}Sampler2D name : register(t{n}) : register(s{linear});")
        elif row.kind == "SeparateImage":
            lines.append(f"#define VT_DECLARE_QUAD_INPUT{n}(name) {vk}Texture2D name : register(t{n});")
    lines.append("")
    lines.append("// Per target: the bundle and the runtime compile define VT_TARGET_METAL for Metal.")
    lines.append("#if defined(VT_TARGET_METAL)")
    lines += metal_only
    lines.append("// 1 where `reference` <= the stored depth (lit), 0 where it is behind it; level 0.")
    lines.append("#define VT_SHADOW_COMPARE(name, uv, reference) name.SampleCmpLevelZero(quadCompareSampler, uv, reference)")
    lines.append("#else")
    lines += other
    lines.append("#define VT_SHADOW_COMPARE(name, uv, reference) ((reference) <= name.SampleLevel(uv, 0.0) ? 1.0 : 0.0)")
    lines.append("#endif")
    return lines


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bindings", required=True, type=Path, help="shaderBindings.h")
    parser.add_argument("--output", required=True, type=Path, help="bindings.slang to write")
    args = parser.parse_args()

    material, scene = read_bindings(args.bindings)
    out = [
        "// Generated by tools/generate_shader_bindings.py from",
        f"// {args.bindings.name}. Do not edit: change the table and rebuild.",
        "//",
        "// Every binding of both backends, as one Slang declaration macro each. register()",
        "// is the Metal slot; [[vk::binding]] the Vulkan and WGSL one. See the header.",
        "",
        "// ---- Material textures (Vulkan set 1, quad inputs 0..5 on both backends) ----",
    ]
    own_sampler_index = 0
    for row in material:
        sampler_slot = None
        if row.own_sampler:
            own_sampler_index += 1
            sampler_slot = own_sampler_index
        out += declaration(row, MATERIAL_SET, sampler_slot)
    out += quad_inputs(material)
    out += ["", "// ---- Scene textures (Vulkan set 3) ----"]
    for row in scene:
        out += declaration(row, SCENE_SET, None)
    out.append("")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(out))


if __name__ == "__main__":
    main()
