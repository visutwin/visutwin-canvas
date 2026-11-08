// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
// One sub-draw of a multi-draw (GraphicsDevice::multiDraw, DrawCommands): a range of a mesh's
// indices (or vertices), its base vertex, an instance count and the first instance. Laid out as
// the 20-byte indexed-indirect record Vulkan (VkDrawIndexedIndirectCommand) and Metal
// (MTLDrawIndexedPrimitivesIndirectArguments) share, in that order, so a list of them is the
// argument buffer of one multi-draw as it stands.
//
#pragma once

#include <cstddef>
#include <cstdint>

#include "scene/mesh.h"

namespace visutwin::canvas
{
    struct MultiDrawCommand
    {
        int32_t count = 0;          // indices (or vertices) to draw
        int32_t instanceCount = 1;
        int32_t first = 0;          // first index (or vertex)
        int32_t baseVertex = 0;     // added to every index
        int32_t firstInstance = 0;
    };
    static_assert(sizeof(MultiDrawCommand) == 20 && offsetof(MultiDrawCommand, instanceCount) == 4 &&
            offsetof(MultiDrawCommand, first) == 8 && offsetof(MultiDrawCommand, baseVertex) == 12 &&
            offsetof(MultiDrawCommand, firstInstance) == 16,
        "MultiDrawCommand is the 20-byte indexed-indirect record");

    /// `primitive` narrowed to the range of `command`, the rest of it kept.
    inline Primitive narrowedPrimitive(const Primitive& primitive, const MultiDrawCommand& command)
    {
        Primitive result = primitive;
        result.base = command.first;
        result.count = command.count;
        result.baseVertex = command.baseVertex;
        result.firstInstance = command.firstInstance;
        return result;
    }
}
