#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2025-2026 Arnis Lektauers
#
# Created by Arnis Lektauers on 07.10.2026
#
"""Reads the binding table in engine/src/platform/graphics/shaderBindings.h.

The two X-macro lists there (VT_MATERIAL_TEXTURE_BINDINGS, VT_SCENE_TEXTURE_BINDINGS) are
the one declaration of every material and scene texture slot on both backends; the
shader-bundle validator and the Slang bindings generator both read them through this
module rather than keeping a table of their own.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

ROW_RE = re.compile(
    r"X\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*([01])\s*,\s*([01])\s*\)"
)
MATERIAL_SET = 1
SCENE_SET = 3

# ShaderBindingKind -> the SPIRV-Cross reflection kind the validator compares.
DESCRIPTOR_KIND = {
    "CombinedSampler": "CombinedImageSampler",
    "SeparateImage": "SampledImage",
    "Sampler": "Sampler",
}


@dataclass(frozen=True)
class BindingRow:
    name: str
    type: str
    kind: str
    metal_slot: int
    vk_binding: int
    own_sampler: bool
    metal_material_slot: bool


def _macro_body(text: str, macro: str) -> str:
    start = text.index(f"#define {macro}(X)")
    end = text.find("\n\n", start)
    return text[start:end if end >= 0 else len(text)]


def read_bindings(header: Path) -> tuple[list[BindingRow], list[BindingRow]]:
    """(material rows, scene rows), each in table order."""
    text = header.read_text()

    def rows(macro: str) -> list[BindingRow]:
        body = _macro_body(text, macro)
        out = []
        for m in ROW_RE.finditer(body):
            name, typ, kind, metal_slot, vk_binding, own, mat = m.groups()
            out.append(BindingRow(name, typ, kind, int(metal_slot), int(vk_binding), own == "1", mat == "1"))
        if not out:
            raise RuntimeError(f"{header}: no rows found in {macro}")
        return out

    return rows("VT_MATERIAL_TEXTURE_BINDINGS"), rows("VT_SCENE_TEXTURE_BINDINGS")


def expected_descriptors(header: Path) -> set[tuple[int, int, str]]:
    """The (set, binding, kind) triples the FORWARD FRAGMENT stage declares in sets 1 and 3.

    Of the material rows that is every fragment material slot (metalMaterialSlot) and the
    shared sampler; quad input 2 is declared by quad passes only and the displacement map
    by the vertex stage, so neither is in the fragment stage's reflection.
    """
    material, scene = read_bindings(header)
    expected: set[tuple[int, int, str]] = set()
    for row in material:
        if row.vk_binding >= 0 and (row.metal_material_slot or row.kind == "Sampler"):
            expected.add((MATERIAL_SET, row.vk_binding, DESCRIPTOR_KIND[row.kind]))
    for row in scene:
        if row.vk_binding >= 0:
            expected.add((SCENE_SET, row.vk_binding, DESCRIPTOR_KIND[row.kind]))
    return expected
