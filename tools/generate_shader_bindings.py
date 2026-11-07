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

from shader_bindings import MATERIAL_SET, SCENE_SET, BindingRow, read_bindings, read_quad_extra_inputs

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
        # The same slot under a name and a combined type of the program's own (Sampler2D,
        # Sampler2DArray): a custom material's textures, which ride the material rows.
        lines.append(f"#define VT_DECLARE_{row.name}_AS(T, name) {vk}T name : register(t{metal_slot}) : register(s{sampler_slot});")
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


def quad_inputs(material: list[BindingRow], extra: list[str]) -> list[str]:
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
            lines.append(f"#define VT_DECLARE_QUAD_ARRAY_INPUT{n}(name) {vk}Sampler2DArray name : register(t{n}) : register(s{linear});")
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
            # An INTEGER texture (PIXELFORMAT_*U / *I), read only with Load: its element type is
            # what each target checks against the format (Metal texture2d<uint>, SPIR-V an image of
            # uint), and a texture of one is nearest-filtered, so its own sampler is a legal one.
            # The input is the n-th material row, so a material draw may declare one too.
            # A MULTISAMPLED texture (TextureOptions::samples), read per sample with Load(coord,
            # sample): a colour one, and a depth one (depth2d_ms on Metal).
            metal_only.append(f"#define VT_DECLARE_QUAD_MS_INPUT{n}(name) {vk}Texture2DMS<float4> name : register(t{n});")
            other.append(f"#define VT_DECLARE_QUAD_MS_INPUT{n}(name) {vk}Sampler2DMS<float4> name : register(t{n}) : register(s{linear});")
            metal_only.append(f"#define VT_DECLARE_QUAD_DEPTH_MS_INPUT{n}(name) {vk}DepthTexture2DMS name : register(t{n});")
            other.append(f"#define VT_DECLARE_QUAD_DEPTH_MS_INPUT{n}(name) {vk}[vk::image_format(\"unknown\")] "
                         f"Sampler2DMS<float> name : register(t{n}) : register(s{linear});")
            for kind, elem in (("UINT", "uint4"), ("INT", "int4")):
                metal_only.append(f"#define VT_DECLARE_QUAD_{kind}_INPUT{n}(name) {vk}Texture2D<{elem}> name : register(t{n});")
                # Slang would type the SPIR-V image by its element (Rgba32ui), which a narrower
                # format (R8_UINT) does not match: the format is left unknown.
                other.append(f"#define VT_DECLARE_QUAD_{kind}_INPUT{n}(name) {vk}[vk::image_format(\"unknown\")] "
                             f"Sampler2D<{elem}> name : register(t{n}) : register(s{linear});")
        elif row.kind == "SeparateImage":
            lines.append(f"#define VT_DECLARE_QUAD_INPUT{n}(name) {vk}Texture2D name : register(t{n});")
            lines.append(f"#define VT_DECLARE_QUAD_ARRAY_INPUT{n}(name) {vk}Texture2DArray name : register(t{n});")
    # Inputs 8 and up: separate images on both backends (Metal texture n, Vulkan the binding of
    # the material row named), read through quadSampler.
    lines.append("// Inputs 8 and up are images read through quadSampler (VT_DECLARE_QUAD_SAMPLER).")
    by_name = {row.name: row for row in material}
    for i, name in enumerate(extra):
        n = 8 + i
        row = by_name[name]
        if row.kind != "SeparateImage":
            raise RuntimeError(f"quad input {n} names {name}, which is not a separate image")
        vk = f"[[vk::binding({row.vk_binding}, {MATERIAL_SET})]] "
        lines.append(f"#define VT_DECLARE_QUAD_INPUT{n}(name) {vk}Texture2D name : register(t{n});")
        lines.append(f"#define VT_DECLARE_QUAD_ARRAY_INPUT{n}(name) {vk}Texture2DArray name : register(t{n});")
    lines.append("")
    lines.append("// Per target: the bundle and the runtime compile define VT_TARGET_METAL for Metal.")
    lines.append("#if defined(VT_TARGET_METAL)")
    lines += metal_only
    lines.append("// One sample of a multisampled input (VT_DECLARE_QUAD_MS_INPUTn / _DEPTH_MS_INPUTn). On")
    lines.append("// Metal, Slang emits read() with an int2 coordinate, which a multisampled texture refuses,")
    lines.append("// so the read is written out; its GetDimensions is wrong there too (get_width(0)): pass the")
    lines.append("// size and the sample count in a uniform.")
    lines.append("float4 vtLoadMS(Texture2DMS<float4> t, int2 c, int s) { __target_switch { case metal: __intrinsic_asm \"$0.read(uint2($1), uint($2))\"; } }")
    lines.append("float vtLoadMS(DepthTexture2DMS t, int2 c, int s) { __target_switch { case metal: __intrinsic_asm \"$0.read(uint2($1), uint($2))\"; } }")
    lines.append("#define VT_LOAD_MS(name, coord, sample) vtLoadMS(name, coord, sample)")
    lines.append("// The bilinear comparison of a `size` x `size` depth map at `uv`: 1 where `reference` <= the")
    lines.append("// stored depth (lit), 0 where it is behind it, blended over the four texels around `uv`; level 0.")
    lines.append("#define VT_SHADOW_COMPARE(name, uv, reference, size) name.SampleCmpLevelZero(quadCompareSampler, uv, reference)")
    lines.append("#else")
    lines += other
    lines.append("#define VT_LOAD_MS(name, coord, sample) (name).Load(coord, sample)")
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


MATERIAL_FIELD_RE = re.compile(r'X\(\s*(vec4x8|vec4|float|uint)\s*,\s*(\w+)\s*,')
SLANG_TYPE_OF_FIELD = {"vec4": "float4", "vec4x8": "float4", "float": "float", "uint": "uint"}
ARRAY_OF_FIELD = {"vec4x8": "[8]"}


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
        lines.append(f"    {SLANG_TYPE_OF_FIELD[shader_type]} {name}{ARRAY_OF_FIELD.get(shader_type, '')};")
    lines += ["};",
              "#define VT_DECLARE_MATERIAL_DATA [[vk::binding(0, 0)]] ConstantBuffer<MaterialData> material : register(b3);"]
    return lines


MAP_LANE_RE = re.compile(r'X\(\s*(\w+)\s*,\s*(\d+)\s*,\s*([01])\s*\)')
CAPACITY_RE = re.compile(r'kBindlessTable(Texture|TextureArray|Volume|Sampler)Capacity\s*=\s*(\d+)')
TABLE_SLOT_RE = re.compile(r'kBindlessTableMetalBufferSlot\s*=\s*(\d+)')
TABLE_SET_RE = re.compile(r'kBindlessTableVulkanSet\s*=\s*(\d+)')
LANES_SLOT_RE = re.compile(r'kBindlessLanesMetalBufferSlot\s*=\s*(\d+)')
LANES_BINDING_RE = re.compile(r'kBindlessLanesVulkanBinding\s*=\s*(\d+)')


def bindless_table(table_header: Path, lanes_header: Path) -> list[str]:
    """The bindless table (bindlessTable.h) every material program declares in place of the
    material map rows, sized by the two capacities there, and the map LANES
    (materialMapLanes.h) a program indexes the material block's mapLanes with. Metal needs
    the arrays sized (an unbounded one is a flexible array member Apple's compiler rejects),
    so the table is one parameter block, samplers first."""
    text = table_header.read_text()
    capacities = dict(CAPACITY_RE.findall(text))
    slot = TABLE_SLOT_RE.search(text)
    vk_set = TABLE_SET_RE.search(text)
    lanes_slot = LANES_SLOT_RE.search(text)
    lanes_binding = LANES_BINDING_RE.search(text)
    for key in ("Texture", "TextureArray", "Volume", "Sampler"):
        if key not in capacities:
            raise RuntimeError(f"{table_header}: kBindlessTable{key}Capacity was not found")
    if not slot or not vk_set or not lanes_slot or not lanes_binding:
        raise RuntimeError(f"{table_header}: the table's slot and set or the lanes' slot and binding were not found")
    lanes = MAP_LANE_RE.findall(lanes_header.read_text())
    if not lanes:
        raise RuntimeError(f"{lanes_header}: no map lanes")
    lines = ["", f"// ---- The bindless table ({table_header.name}): Metal buffer {slot.group(1)} on both stages,",
             f"// Vulkan set {vk_set.group(1)}; the samplers at binding 0, the 2D textures at 1, the texture",
             "// arrays at 2 and the volumes at 3, each kind an index space of its own. ----",
             "struct VtBindlessTable",
             "{",
             f"    SamplerState samplers[{capacities['Sampler']}];",
             f"    Texture2D textures[{capacities['Texture']}];",
             f"    Texture2DArray textureArrays[{capacities['TextureArray']}];",
             f"    Texture3D volumes[{capacities['Volume']}];",
             "};",
             f"#define VT_DECLARE_BINDLESS_TABLE [[vk::binding(0, {vk_set.group(1)})]] "
             f"ParameterBlock<VtBindlessTable> vtBindless : register(b{slot.group(1)});",
             "// A lane: the texture index in the low 24 bits (in the array the program names), the",
             "// sampler index above.",
             "#define VT_BINDLESS_TEXTURE(lane) vtBindless.textures[(lane) & 0xFFFFFFu]",
             "#define VT_BINDLESS_TEXTURE_ARRAY(lane) vtBindless.textureArrays[(lane) & 0xFFFFFFu]",
             "#define VT_BINDLESS_VOLUME(lane) vtBindless.volumes[(lane) & 0xFFFFFFu]",
             "#define VT_BINDLESS_SAMPLER(lane) vtBindless.samplers[(lane) >> 24u]",
             f"// ---- The draw's map lanes ({lanes_header.name}): Metal buffer {lanes_slot.group(1)} on both",
             f"// stages, Vulkan set 0 binding {lanes_binding.group(1)}; lane i at [i / 4][i & 3], 0 for no map. ----",
             "struct VtMapLanes",
             "{",
             "    uint4 lanes[7];",
             "};",
             f"#define VT_DECLARE_MAP_LANES [[vk::binding({lanes_binding.group(1)}, 0)]] "
             f"ConstantBuffer<VtMapLanes> vtMapLanes : register(b{lanes_slot.group(1)});",
             "#define VT_MATERIAL_LANE(i) vtMapLanes.lanes[(i) >> 2u][(i) & 3u]",
             "// A map read through its lane, for a program with a block of its own: the 2D sample",
             "// with the lane's sampler, an array's and a volume's with a float3 coordinate.",
             "#define VT_MAP_SAMPLE(laneIndex, uv) VT_BINDLESS_TEXTURE(VT_MATERIAL_LANE(laneIndex)).Sample(VT_BINDLESS_SAMPLER(VT_MATERIAL_LANE(laneIndex)), uv)",
             "#define VT_MAP_SAMPLE_LEVEL(laneIndex, uv, lod) VT_BINDLESS_TEXTURE(VT_MATERIAL_LANE(laneIndex)).SampleLevel(VT_BINDLESS_SAMPLER(VT_MATERIAL_LANE(laneIndex)), uv, lod)",
             "#define VT_MAP_ARRAY_SAMPLE(laneIndex, uvw) VT_BINDLESS_TEXTURE_ARRAY(VT_MATERIAL_LANE(laneIndex)).Sample(VT_BINDLESS_SAMPLER(VT_MATERIAL_LANE(laneIndex)), uvw)",
             "#define VT_MAP_ARRAY_SAMPLE_LEVEL(laneIndex, uvw, lod) VT_BINDLESS_TEXTURE_ARRAY(VT_MATERIAL_LANE(laneIndex)).SampleLevel(VT_BINDLESS_SAMPLER(VT_MATERIAL_LANE(laneIndex)), uvw, lod)",
             "#define VT_MAP_VOLUME_SAMPLE_LEVEL(laneIndex, uvw, lod) VT_BINDLESS_VOLUME(VT_MATERIAL_LANE(laneIndex)).SampleLevel(VT_BINDLESS_SAMPLER(VT_MATERIAL_LANE(laneIndex)), uvw, lod)",
             f"// ---- The map lanes ({lanes_header.name}), by the map's name ----"]
    for index, (name, _slot, _own) in enumerate(lanes):
        lines.append(f"#define VT_MAP_LANE_{name} {index}u")
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
    parser.add_argument("--bindless", type=Path,
                        help="bindlessTable.h; with --map-lanes emits the bindless table and the map lanes")
    parser.add_argument("--map-lanes", type=Path, help="materialMapLanes.h")
    args = parser.parse_args()

    material, scene = read_bindings(args.bindings)
    out = [
        "// Generated by tools/generate_shader_bindings.py from",
        f"// {args.bindings.name}. Do not edit: change the table and rebuild.",
        "//",
        "// Every binding of both backends, as one Slang declaration macro each. register()",
        "// is the Metal slot; [[vk::binding]] the Vulkan and WGSL one. See the header.",
        "",
        "// ---- The material set (Vulkan set 1): the quad inputs and the shared sampler ----",
    ]
    # The material rows are the quad inputs (quad_inputs below) and the shared sampler; a
    # material's maps come from the bindless table, so no row of a texture is declared by name.
    for row in material:
        if row.type == "Sampler":
            out += declaration(row, MATERIAL_SET, None)
    out += quad_inputs(material, read_quad_extra_inputs(args.bindings))
    out += ["", "// ---- Scene textures (Vulkan set 3) ----"]
    for row in scene:
        out += declaration(row, SCENE_SET, None)
    if args.material_fields:
        out += material_block(args.material_fields)
    if args.bindless and args.map_lanes:
        out += bindless_table(args.bindless, args.map_lanes)
    if args.features:
        out += feature_words(args.features)
    out.append("")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(out))


if __name__ == "__main__":
    main()
