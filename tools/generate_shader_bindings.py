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
import re
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
                # the combined sampler gathered and compared by hand, with the same bilinear blend
                # (no comparison sampler fits MoltenVK's 16-sampler budget). It must be DepthTexture2D on Metal: a Texture2D
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
    lines.append("// The bilinear comparison of a `size` x `size` depth map at `uv`: 1 where `reference` <= the")
    lines.append("// stored depth (lit), 0 where it is behind it, blended over the four texels around `uv`; level 0.")
    lines.append("#define VT_SHADOW_COMPARE(name, uv, reference, size) name.SampleCmpLevelZero(quadCompareSampler, uv, reference)")
    lines.append("#else")
    lines += other
    lines.append("// The comparison sampler done by hand: gather the four texels at the corner they share and")
    lines.append("// blend their comparisons by the tap's bilinear fraction, as the forward pass's shadow taps do.")
    lines.append("float vtShadowCompareGather(Sampler2D<float> map, float2 uv, float reference, float size)")
    lines.append("{")
    lines.append("    const float2 t = uv * size - 0.5;")
    lines.append("    const float2 base = floor(t);")
    lines.append("    const float2 f = t - base;")
    lines.append("    // Gather order: x (i0, j1), y (i1, j1), z (i1, j0), w (i0, j0).")
    lines.append("    const float4 lit = step(float4(reference), map.GatherRed((base + 1.0) / size));")
    lines.append("    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);")
    lines.append("}")
    lines.append("#define VT_SHADOW_COMPARE(name, uv, reference, size) vtShadowCompareGather(name, uv, reference, size)")
    lines.append("#endif")
    return lines


MATERIAL_FIELD_RE = re.compile(r'X\(\s*(vec4|float|uint)\s*,\s*(\w+)\s*,')
SLANG_TYPE_OF_FIELD = {"vec4": "float4", "float": "float", "uint": "uint"}


def material_block(fields_header: Path) -> list[str]:
    """`struct MaterialData` from the X-macro field list C++, MSL and GLSL expand from, and
    VT_DECLARE_MATERIAL_DATA declaring it at the per-draw material slot (Metal buffer 3,
    Vulkan set 0 binding 0). Every vec4 in the list lands 16-aligned by construction, so the
    one struct has the same layout as std140 and as Metal's natural layout."""
    fields = MATERIAL_FIELD_RE.findall(fields_header.read_text())
    if not fields:
        raise RuntimeError(f"{fields_header}: no material uniform fields")
    lines = ["", f"// ---- The per-draw material block ({fields_header.name}, {len(fields)} fields) ----",
             "struct MaterialData", "{"]
    for shader_type, name in fields:
        lines.append(f"    {SLANG_TYPE_OF_FIELD[shader_type]} {name};")
    lines += ["};",
              "#define VT_DECLARE_MATERIAL_DATA [[vk::binding(0, 0)]] ConstantBuffer<MaterialData> material : register(b3);"]
    return lines


FEATURE_RE = re.compile(r'X\(\s*(\w+)\s*,\s*"(VT_FEATURE_\w+)"\s*\)')


def feature_words(features_header: Path) -> list[str]:
    """The feature words as specialization constants (Vulkan) / function constants (Metal),
    one per 32 features, ids 0..words-1, in declaration order: the same layout as
    ShaderFeatureSet and the Vulkan bundle's shader_features.glsl. Plus vtFeatureEnabled
    and one VT_FEATURE_<NAME>_BIT constant per feature."""
    features = FEATURE_RE.findall(features_header.read_text())
    if not features:
        raise RuntimeError(f"{features_header}: no shader features")
    words = (len(features) + 31) // 32
    lines = ["", f"// ---- Shader features ({features_header.name}: {len(features)} in {words} word(s)) ----"]
    for word in range(words):
        lines.append(f"[[vk::constant_id({word})]] const uint vtFeatureMask{word} = 0;")
    lines += ["bool vtFeatureEnabled(uint bit)", "{", "    const uint mask = 1u << (bit & 31u);",
              "    const uint word = bit >> 5u;"]
    for word in range(words):
        lines.append(f"    if (word == {word}u) {{ return (vtFeatureMask{word} & mask) != 0u; }}")
    lines += ["    return false;", "}"]
    for index, (_symbol, define_name) in enumerate(features):
        lines.append(f"static const uint {define_name}_BIT = {index}u;")
    return lines


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bindings", required=True, type=Path, help="shaderBindings.h")
    parser.add_argument("--output", required=True, type=Path, help="bindings.slang to write")
    parser.add_argument("--material-fields", type=Path,
                        help="materialUniformFields.h; emits struct MaterialData when given")
    parser.add_argument("--features", type=Path,
                        help="shaderFeatures.h; emits the feature words when given")
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
    if args.material_fields:
        out += material_block(args.material_fields)
    if args.features:
        out += feature_words(args.features)
    out.append("")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(out))


if __name__ == "__main__":
    main()
