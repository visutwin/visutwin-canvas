#!/usr/bin/env python3
"""Compile Vulkan GLSL at build time, reflect it, and emit a C++ bundle."""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
from pathlib import Path


MODULES = (
    ("ForwardVert", "forward.vert", "vert"),
    ("ForwardInstancedVert", "forward_instanced.vert", "vert"),
    ("ForwardSkyVert", "forward_sky.vert", "vert"),
    ("ForwardColorVert", "forward_color.vert", "vert"),
    ("ForwardPointVert", "forward_point.vert", "vert"),
    ("ForwardFrag", "forward.frag", "frag"),
    ("ShadowVsmFrag", "shadow_vsm_moments.frag", "frag"),
    ("VsmBlurVert", "vsm_blur.vert", "vert"),
    ("VsmBlurFrag", "vsm_blur.frag", "frag"),
    ("PostFullscreenVert", "post_fullscreen.vert", "vert"),
    ("PostComposeFrag", "post_compose.frag", "frag"),
    ("PostSsaoFrag", "post_ssao.frag", "frag"),
    ("PostDepthBlurFrag", "post_depth_blur.frag", "frag"),
    ("PostTaaFrag", "post_taa.frag", "frag"),
)

FEATURE_RE = re.compile(
    r'X\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*(\d+)\s*\)'
)


def run(command: list[str]) -> str:
    result = subprocess.run(
        command, check=False, text=True, capture_output=True
    )
    if result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout}{result.stderr}"
        )
    return result.stdout


def reflect(spirv_cross: str, path: Path) -> dict:
    return json.loads(run([spirv_cross, str(path), "--reflect"]))


def resources(reflection: dict) -> list[tuple[int, int, str, int]]:
    result: list[tuple[int, int, str, int]] = []
    for kind, reflected_kind in (
        ("ubos", "UniformBuffer"),
        ("textures", "CombinedImageSampler"),
        ("sampled_images", "CombinedImageSampler"),
        ("storage_buffers", "StorageBuffer"),
        ("storage_images", "StorageImage"),
    ):
        for item in reflection.get(kind, []):
            result.append(
                (
                    int(item["set"]),
                    int(item["binding"]),
                    reflected_kind,
                    int(item.get("block_size", 0)),
                )
            )
    return sorted(result)


def push_constant_size(reflection: dict) -> int:
    constants = reflection.get("push_constants", [])
    if not constants:
        return 0
    if len(constants) != 1:
        raise RuntimeError("only one push-constant block is supported")
    item = constants[0]
    if "block_size" in item:
        return int(item["block_size"])

    def type_size(type_name: str) -> int:
        scalar_sizes = {
            "float": 4, "int": 4, "uint": 4,
            "vec2": 8, "ivec2": 8, "uvec2": 8,
            "vec3": 12, "ivec3": 12, "uvec3": 12,
            "vec4": 16, "ivec4": 16, "uvec4": 16,
            "mat4": 64,
        }
        if type_name in scalar_sizes:
            return scalar_sizes[type_name]
        reflected_type = reflection.get("types", {}).get(type_name)
        if not reflected_type:
            raise RuntimeError(f"cannot size reflected type {type_name}")
        extent = 0
        for member in reflected_type.get("members", []):
            member_size = type_size(member["type"])
            if "matrix_stride" in member:
                member_size = int(member["matrix_stride"]) * 4
            if member.get("array"):
                member_size = int(member["array_stride"]) * int(member["array"][0])
            extent = max(extent, int(member["offset"]) + member_size)
        return extent

    return type_size(item["type"])


def validate(module: str, reflection: dict) -> None:
    bindings = resources(reflection)
    if module == "PostFullscreenVert":
        if bindings or push_constant_size(reflection):
            raise RuntimeError(f"{module}: unexpected reflected resources")
        return
    if module.startswith("Post") and module.endswith("Frag"):
        for set_index, binding, kind, _ in bindings:
            valid = (
                set_index == 0
                and (
                    (binding < 4 and kind == "CombinedImageSampler")
                    or (binding == 4 and kind == "UniformBuffer")
                )
            )
            if not valid:
                raise RuntimeError(
                    f"{module}: incompatible post descriptor {bindings}"
                )
        if not any(binding == 4 for _, binding, _, _ in bindings):
            raise RuntimeError(f"{module}: params UBO was not reflected")
        if push_constant_size(reflection):
            raise RuntimeError(f"{module}: unexpected push constants")
        return
    if module == "VsmBlurVert":
        if bindings or push_constant_size(reflection):
            raise RuntimeError(f"{module}: unexpected reflected resources")
        return
    if module == "VsmBlurFrag":
        expected = [(0, 0, "CombinedImageSampler", 0)]
        if bindings != expected or push_constant_size(reflection) != 32:
            raise RuntimeError(
                f"{module}: reflected layout mismatch: "
                f"bindings={bindings}, push={push_constant_size(reflection)}"
            )
        return
    if module.endswith("Vert"):
        if push_constant_size(reflection) != 128:
            raise RuntimeError(
                f"{module}: reflected push constants are not 128 bytes"
            )
        if bindings:
            raise RuntimeError(f"{module}: unexpected descriptors: {bindings}")
        return
    if module == "ShadowVsmFrag":
        if bindings or push_constant_size(reflection):
            raise RuntimeError(
                f"{module}: shadow moments stage unexpectedly owns resources"
            )
        return

    expected = {
        (0, 0, "UniformBuffer"),
        (2, 0, "UniformBuffer"),
        (1, 0, "CombinedImageSampler"),
        (1, 1, "CombinedImageSampler"),
        (1, 3, "CombinedImageSampler"),
        (1, 4, "CombinedImageSampler"),
        (1, 5, "CombinedImageSampler"),
        *( (3, binding, "CombinedImageSampler") for binding in range(7) ),
    }
    actual = {(set_index, binding, kind) for set_index, binding, kind, _ in bindings}
    if actual != expected:
        raise RuntimeError(
            f"{module}: descriptor reflection mismatch\n"
            f"expected={sorted(expected)}\nactual={sorted(actual)}"
        )
    block_sizes = {
        (set_index, binding): size
        for set_index, binding, kind, size in bindings
        if kind == "UniformBuffer"
    }
    if block_sizes != {(0, 0): 224, (2, 0): 1120}:
        raise RuntimeError(
            f"{module}: uniform block reflection mismatch: {block_sizes}"
        )


def stage_interface(reflection: dict, direction: str) -> list[tuple[int, str]]:
    return sorted(
        (int(item["location"]), str(item["type"]))
        for item in reflection.get(direction, [])
        if "location" in item
    )


def words(path: Path) -> list[int]:
    data = path.read_bytes()
    if len(data) % 4:
        raise RuntimeError(f"{path}: SPIR-V byte count is not word aligned")
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def emit_array(output: list[str], name: str, values: list[int]) -> None:
    output.append(f"inline constexpr uint32_t k{name}[] = {{")
    for start in range(0, len(values), 8):
        row = ", ".join(f"0x{value:08x}u" for value in values[start:start + 8])
        output.append(f"    {row},")
    output.append("};")
    output.append(
        f"inline constexpr size_t k{name}WordCount = "
        f"sizeof(k{name}) / sizeof(uint32_t);"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--glslc", required=True)
    parser.add_argument("--spirv-cross", required=True)
    parser.add_argument("--shader-dir", type=Path, required=True)
    parser.add_argument("--features", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()

    features = FEATURE_RE.findall(args.features.read_text())
    if not features:
        raise RuntimeError("shared shader feature list is empty")

    args.work_dir.mkdir(parents=True, exist_ok=True)
    feature_glsl = args.work_dir / "shader_features.glsl"
    feature_lines = [
        "// Generated from platform/graphics/shaderFeatures.h.",
        "layout(constant_id = 0) const uint vtFeatureMaskLo = 0u;",
        "layout(constant_id = 1) const uint vtFeatureMaskHi = 0u;",
        "bool vtFeatureEnabled(uint bit) {",
        "    return bit < 32u",
        "        ? (vtFeatureMaskLo & (1u << bit)) != 0u",
        "        : (vtFeatureMaskHi & (1u << (bit - 32u))) != 0u;",
        "}",
    ]
    for symbol, define_name, bit in features:
        feature_lines.append(
            f"const uint {define_name}_BIT = {int(bit)}u;"
        )
    feature_glsl.write_text("\n".join(feature_lines) + "\n")

    reflected: dict[str, dict] = {}
    blobs: dict[str, list[int]] = {}
    for module, filename, stage in MODULES:
        source = args.shader_dir / filename
        destination = args.work_dir / f"{module}.spv"
        run(
            [
                args.glslc,
                "--target-env=vulkan1.3",
                "--target-spv=spv1.3",
                f"-fshader-stage={stage}",
                f"-I{args.work_dir}",
                str(source),
                "-o",
                str(destination),
            ]
        )
        module_reflection = reflect(args.spirv_cross, destination)
        validate(module, module_reflection)
        reflected[module] = module_reflection
        blobs[module] = words(destination)

    fragment_inputs = stage_interface(reflected["ForwardFrag"], "inputs")
    for vertex_module in (
        "ForwardVert",
        "ForwardInstancedVert",
        "ForwardSkyVert",
        "ForwardColorVert",
        "ForwardPointVert",
    ):
        vertex_outputs = stage_interface(reflected[vertex_module], "outputs")
        if vertex_outputs != fragment_inputs:
            raise RuntimeError(
                f"{vertex_module}: stage interface does not match ForwardFrag\n"
                f"vertex={vertex_outputs}\nfragment={fragment_inputs}"
            )

    output = [
        "// Generated file. Do not edit.",
        "#pragma once",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace visutwin::canvas::vulkan_generated {",
        "enum class ReflectedDescriptorKind : uint8_t {",
        "    UniformBuffer, CombinedImageSampler, StorageBuffer, StorageImage",
        "};",
        "struct ReflectedBinding {",
        "    uint8_t set;",
        "    uint8_t binding;",
        "    ReflectedDescriptorKind kind;",
        "    uint32_t blockSize;",
        "};",
        "",
    ]
    for module, _, _ in MODULES:
        emit_array(output, module, blobs[module])
        output.append(
            f"inline constexpr uint32_t k{module}PushConstantSize = "
            f"{push_constant_size(reflected[module])}u;"
        )
        module_resources = resources(reflected[module])
        output.append(
            f"inline constexpr std::array<ReflectedBinding, "
            f"{len(module_resources)}> k{module}Bindings = {{{{"
        )
        for set_index, binding, kind, block_size in module_resources:
            output.append(
                "    {"
                f"{set_index}, {binding}, ReflectedDescriptorKind::{kind}, "
                f"{block_size}u"
                "},"
            )
        output.append("}};")
        output.append("")
    output.append("} // namespace visutwin::canvas::vulkan_generated")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(output) + "\n")


if __name__ == "__main__":
    main()
