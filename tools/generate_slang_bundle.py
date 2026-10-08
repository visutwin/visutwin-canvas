#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2025-2026 Arnis Lektauers
#
# Created by Arnis Lektauers on 07.10.2026
#
"""Compiles every Slang program into one embedded header: slang_shader_bundle.h.

For each engine/shaders/slang/programs/<name>.slang the entry points are read from its
`[shader("vertex")]` / `[shader("fragment")]` / `[shader("compute")]` attributes and the
program is compiled with slangc, once per target this build's backends take: `metal`
(one MSL source holding every entry point, compiled by the Metal backend at run time the
way its hand-written MSL is) and `spirv` (one module per entry point, the entry point
named `main` as the Vulkan pipeline binds it). Matrices are read ROW-major on every target, so
a shader writes `mul(v, M)` for the engine's column-major M * v (see SlangCompiler).
The generated bindings.slang is on the include path, and its text is embedded too so a
runtime override compile can serve it from memory.

The header holds one `Program` per source with its name, entry point names and the code
for each target that was built (empty where it was not), and `findProgram(name)`. With the
SPIR-V target each entry also carries its REFLECTED layout: every descriptor the entry's
module actually uses (set, binding, kind, and a uniform block's size) and its push-constant
size, from slangc's reflection JSON joined to the entry's own SPIR-V. The Vulkan backend
checks the forward and shadow entries against its descriptor contract with them, and C++
asserts the shared blocks' sizes against its structs.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

# `[shader("stage")]`, any further attributes (a compute entry's `[numthreads(...)]`), the
# return type, the name.
ENTRY_RE = re.compile(r'\[shader\("(vertex|fragment|compute)"\)\]\s*(?:\[[^\]]*\]\s*)*[\w<>:]+\s+(\w+)\s*\(')
# `// @variant <name>: DEFINE=value DEFINE2` — one compile per line, with those defines; a
# program with no such line is compiled once, as variant "".
VARIANT_RE = re.compile(r'^//\s*@variant\s+([\w.-]+)\s*:(.*)$', re.MULTILINE)


def variants_of(source: str) -> list[tuple[str, list[str]]]:
    found = [(m.group(1), m.group(2).split()) for m in VARIANT_RE.finditer(source)]
    return found if found else [("", [])]
STAGE_FLAG = {"vertex": "vertex", "fragment": "fragment", "compute": "compute"}


INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def entry_points(program: Path, include_dirs: list[Path]) -> list[tuple[str, str]]:
    """(stage, entry) for every [shader(...)] function in `program` and in every file it
    #includes (resolved next to the including file, then on `include_dirs`), in source
    order. A program composed from chunks keeps its entry points in them."""
    found: list[tuple[str, str]] = []
    seen: set[Path] = set()

    def visit(path: Path) -> None:
        path = path.resolve()
        if path in seen:
            return
        seen.add(path)
        text = path.read_text()
        for match in INCLUDE_RE.finditer(text):
            name = match.group(1)
            for base in [path.parent, *include_dirs]:
                candidate = base / name
                if candidate.exists():
                    visit(candidate)
                    break
        found.extend(ENTRY_RE.findall(text))

    visit(program)
    return found


# Reflection-JSON type kind -> the descriptor kind the C++ side names.
def descriptor_kind(type_info: dict) -> str:
    kind = type_info.get("kind")
    if kind == "constantBuffer":
        return "UniformBuffer"
    if kind == "samplerState":
        return "Sampler"
    if kind == "resource":
        shape = type_info.get("baseShape", "")
        if shape in ("structuredBuffer", "byteAddressBuffer"):
            return "StorageBuffer"
        return "CombinedImageSampler" if type_info.get("combined") else "SampledImage"
    raise RuntimeError(f"unhandled reflected parameter kind {kind}")


def reflected_parameters(json_path: Path) -> dict[str, tuple[str, int]]:
    """name -> (descriptor kind, uniform block bytes or 0) for every program parameter, and
    "@push" -> ("PushConstant", bytes) for the push-constant block."""
    data = json.loads(json_path.read_text())
    out: dict[str, tuple[str, int]] = {}
    for param in data["parameters"]:
        binding = param.get("binding", {})
        type_info = param["type"]
        if binding.get("kind") == "specializationConstant":
            continue
        size = 0
        if type_info.get("kind") == "constantBuffer":
            size = int(type_info.get("elementVarLayout", {}).get("binding", {}).get("size", 0))
        if binding.get("kind") == "pushConstantBuffer":
            out["@push"] = ("PushConstant", size)
            continue
        out[param["name"]] = (descriptor_kind(type_info), size)
    return out


def used_descriptors(spirv: bytes) -> tuple[list[tuple[str, int, int]], list[str]]:
    """(name, set, binding) of every descriptor variable a SPIR-V module declares (Slang
    drops the globals an entry does not use), and the names of its push-constant variables."""
    words = struct.unpack(f"<{len(spirv) // 4}I", spirv)
    names: dict[int, str] = {}
    sets: dict[int, int] = {}
    bindings: dict[int, int] = {}
    push: list[int] = []
    i = 5
    while i < len(words):
        opcode = words[i] & 0xFFFF
        count = words[i] >> 16
        args = words[i + 1:i + count]
        if opcode == 5:   # OpName
            raw = b"".join(struct.pack("<I", w) for w in args[1:])
            names[args[0]] = raw.split(b"\0")[0].decode()
        elif opcode == 71 and len(args) >= 3:   # OpDecorate
            if args[1] == 34:   # DescriptorSet
                sets[args[0]] = args[2]
            elif args[1] == 33:   # Binding
                bindings[args[0]] = args[2]
        elif opcode == 59 and len(args) >= 3 and args[2] == 9:   # OpVariable, PushConstant
            push.append(args[1])
        i += count
    used = sorted(((names.get(v, ""), sets[v], bindings[v]) for v in sets if v in bindings),
                  key=lambda u: (u[1], u[2]))
    return used, [names.get(v, "") for v in push]


def run(command: list[str]) -> None:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise RuntimeError(f"command failed: {' '.join(command)}")
    if result.stderr.strip():
        # slangc warnings go to stderr with a zero exit; keep them visible in the build log.
        sys.stderr.write(result.stderr)


def camel(name: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in re.split(r"[-_]", name))


def words_literal(data: bytes) -> str:
    if len(data) % 4:
        raise RuntimeError("SPIR-V is not a whole number of words")
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    lines = []
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join(f"0x{w:08x}u" for w in words[i:i + 8]) + ",")
    return "\n".join(lines)


def raw_string(text: str) -> str:
    delimiter = "VTSLANG"
    while f"){delimiter}\"" in text:
        delimiter += "_"
    return f'R"{delimiter}({text}){delimiter}"'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slangc", required=True, type=Path)
    parser.add_argument("--source-dir", required=True, type=Path, help="engine/shaders/slang")
    parser.add_argument("--bindings", required=True, type=Path, help="the generated bindings.slang")
    parser.add_argument("--targets", required=True, help="comma-separated: metal, spirv")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()

    targets = [t for t in args.targets.split(",") if t]
    for target in targets:
        if target not in ("metal", "spirv"):
            raise RuntimeError(f"unknown target {target}")
    args.work_dir.mkdir(parents=True, exist_ok=True)
    modules_dir = args.source_dir / "modules"
    programs = sorted((args.source_dir / "programs").glob("*.slang"))

    common = [str(args.slangc), "-I", str(args.source_dir), "-I", str(modules_dir), "-I", str(args.bindings.parent),
              "-matrix-layout-row-major"]

    out = [
        "// Generated by tools/generate_slang_bundle.py from engine/shaders/slang. Do not edit.",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace visutwin::canvas::slang_generated",
        "{",
        "    enum class DescriptorKind : uint8_t",
        "    {",
        "        UniformBuffer, StorageBuffer, CombinedImageSampler, SampledImage, Sampler",
        "    };",
        "",
        "    struct ReflectedBinding",
        "    {",
        "        const char* name;",
        "        uint32_t set;",
        "        uint32_t binding;",
        "        DescriptorKind kind;",
        "        uint32_t blockBytes;        // a uniform block's size; 0 otherwise",
        "    };",
        "",
        "    struct Entry",
        "    {",
        "        const char* stage;          // \"vertex\", \"fragment\" or \"compute\"",
        "        const char* name;",
        "        const uint32_t* spirv;      // this entry alone, compiled to SPIR-V; null when not built",
        "        size_t spirvWords;",
        "        const ReflectedBinding* bindings;   // the descriptors this entry's SPIR-V uses",
        "        size_t bindingCount;",
        "        uint32_t pushConstantBytes; // 0 when the entry reads no push constants",
        "    };",
        "",
        "    struct Program",
        "    {",
        "        const char* name;",
        "        const char* variant;        // \"\" for a program without @variant lines",
        "        const char* vertexEntry;    // empty when the program has no such stage",
        "        const char* fragmentEntry;",
        "        const char* computeEntry;",
        "        const char* metalSource;    // MSL with every entry point; empty when metal was not built",
        "        const uint8_t* metalLibrary; // the same compiled to a metallib by Apple's compiler; null when not built",
        "        size_t metalLibraryBytes;",
        "        const uint32_t* vertexSpirv;",
        "        size_t vertexSpirvWords;",
        "        const uint32_t* fragmentSpirv;",
        "        size_t fragmentSpirvWords;",
        "        const uint32_t* computeSpirv;",
        "        size_t computeSpirvWords;",
        "        const struct Entry* entries; // every entry point, in source order (a program may have several per stage)",
        "        size_t entryCount;",
        "    };",
        "",
        f"    inline constexpr char kBindingsSlang[] = {raw_string(args.bindings.read_text())};",
        "",
    ]
    entries = []
    for program in programs:
        name = program.stem
        source = program.read_text()
        found_list = entry_points(program, [args.source_dir, modules_dir, args.bindings.parent])
        found = {}
        for stage, entry in found_list:
            found.setdefault(stage, entry)
        if not found:
            raise RuntimeError(f"{program}: no [shader(...)] entry points found")
        for variant, defines in variants_of(source):
            define_flags = []
            for define in defines:
                define_flags += ["-D" + define]
            ident = "k" + camel(name) + (camel(variant.replace(".", "_")) if variant else "")
            stem = name + (f"@{variant}" if variant else "")
            fields = {"metalSource": '""', "metalLibrary": "nullptr, 0"}
            spirv = {}
            if "metal" in targets:
                msl_path = args.work_dir / f"{stem}.metal"
                command = common + define_flags + ["-DVT_TARGET_METAL"] + [str(program), "-target", "metal", "-o", str(msl_path)]
                for stage, entry in found_list:
                    command += ["-entry", entry, "-stage", STAGE_FLAG[stage]]
                run(command)
                out.append(f"    inline constexpr char {ident}Msl[] = {raw_string(msl_path.read_text())};")
                fields["metalSource"] = f"{ident}Msl"
                # The same MSL compiled by Apple's compiler, so the Metal backend loads a library
                # instead of compiling MSL on first use. Compiled HERE, with fast math on, as
                # MetalShader compiles source at run time, not through slangc's metallib
                # target: slangc invokes the compiler with options of its own, and the library it
                # built rounded differently from the runtime compile of the very same MSL (TAA
                # by a count over 44,000 pixels), so the bundle and an override disagreed.
                air_path = args.work_dir / f"{stem}.air"
                lib_path = args.work_dir / f"{stem}.metallib"
                run(["xcrun", "-sdk", "macosx", "metal", "-ffast-math", "-c", str(msl_path), "-o", str(air_path)])
                run(["xcrun", "-sdk", "macosx", "metallib", str(air_path), "-o", str(lib_path)])
                data = lib_path.read_bytes()
                if data[:4] != b"MTLB":
                    raise RuntimeError(f"{lib_path}: not a metallib")
                out.append(f"    inline constexpr uint8_t {ident}MetalLib[] = {{")
                for i in range(0, len(data), 24):
                    out.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
                out.append("    };")
                fields["metalLibrary"] = f"{ident}MetalLib, sizeof({ident}MetalLib)"
            entry_arrays = {}
            entry_layouts = {}
            if "spirv" in targets:
                reflection_path = args.work_dir / f"{stem}.reflection.json"
                parameters: dict[str, tuple[str, int]] = {}
                for index, (stage, entry) in enumerate(found_list):
                    spv_path = args.work_dir / f"{stem}.{entry}.spv"
                    command = common + define_flags + [str(program), "-target", "spirv", "-entry", entry,
                                                       "-stage", STAGE_FLAG[stage], "-o", str(spv_path)]
                    if index == 0:
                        # The program's parameters (names, kinds, block sizes); one is enough.
                        command += ["-reflection-json", str(reflection_path)]
                    run(command)
                    if index == 0:
                        parameters = reflected_parameters(reflection_path)
                    data = spv_path.read_bytes()
                    if data[:4] != b"\x03\x02\x23\x07":
                        raise RuntimeError(f"{spv_path}: not SPIR-V")
                    array = f"{ident}{camel(entry)}Spirv"
                    out.append(f"    inline constexpr uint32_t {array}[] = {{")
                    out.append(words_literal(data))
                    out.append("    };")
                    entry_arrays[entry] = array
                    spirv.setdefault(stage, array)

                    used, push = used_descriptors(data)
                    rows = []
                    for binding_name, set_index, binding in used:
                        if binding_name not in parameters:
                            raise RuntimeError(f"{program}:{entry}: descriptor '{binding_name}' is not in the reflection")
                        kind, size = parameters[binding_name]
                        rows.append(f'        ReflectedBinding{{"{binding_name}", {set_index}u, {binding}u, '
                                    f'DescriptorKind::{kind}, {size}u}},')
                    push_bytes = parameters.get("@push", ("PushConstant", 0))[1] if push else 0
                    bindings_array = f"{ident}{camel(entry)}Bindings"
                    if rows:
                        out.append(f"    inline constexpr ReflectedBinding {bindings_array}[] = {{")
                        out.extend(rows)
                        out.append("    };")
                        entry_layouts[entry] = (f"{bindings_array}, {len(rows)}", push_bytes)
                    else:
                        entry_layouts[entry] = ("nullptr, 0", push_bytes)
            out.append(f"    inline constexpr Entry {ident}Entries[] = {{")
            for stage, entry in found_list:
                array = entry_arrays.get(entry)
                spv = f"{array}, sizeof({array}) / sizeof(uint32_t)" if array else "nullptr, 0"
                layout, push_bytes = entry_layouts.get(entry, ("nullptr, 0", 0))
                out.append(f'        Entry{{"{stage}", "{entry}", {spv}, {layout}, {push_bytes}u}},')
            out.append("    };")
            entry_names = {stage: f'"{found[stage]}"' if stage in found else '""'
                           for stage in ("vertex", "fragment", "compute")}
            spv_fields = []
            for stage in ("vertex", "fragment", "compute"):
                if stage in spirv:
                    spv_fields.append(f"{spirv[stage]}, sizeof({spirv[stage]}) / sizeof(uint32_t)")
                else:
                    spv_fields.append("nullptr, 0")
            entries.append(
                f'        Program{{"{name}", "{variant}", {entry_names["vertex"]}, {entry_names["fragment"]}, '
                f'{entry_names["compute"]}, {fields["metalSource"]}, {fields["metalLibrary"]}, {", ".join(spv_fields)}, '
                f'{ident}Entries, {len(found_list)}}},')
            out.append("")

    out += [
        f"    inline constexpr std::array<Program, {len(entries)}> kPrograms = {{{{",
        *entries,
        "    }};",
        "",
        "    inline const Program* findProgram(const std::string_view name, const std::string_view variant = {})",
        "    {",
        "        for (const auto& program : kPrograms) {",
        "            if (name == program.name && variant == program.variant) {",
        "                return &program;",
        "            }",
        "        }",
        "        return nullptr;",
        "    }",
        "}",
        "",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(out))


if __name__ == "__main__":
    main()
