// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// A list of sub-draws of one mesh instance, drawn in place of its whole primitive
// (MeshInstance::setMultiDraw): each names a range of the mesh's indices (or vertices), its
// base vertex, an instance count and the first instance. The application fills slots with add()
// and says how many are live with update(); the list persists until it is changed.
//
// The list is issued by GraphicsDevice::multiDraw: one indirect draw call over the list where
// the device supports it (supportsMultiDrawIndirect: Vulkan), otherwise a draw call per sub-draw
// with the state bound once (Metal, which has no multi-draw call).
//
#pragma once

#include <algorithm>
#include <vector>

#include "mesh.h"
#include "platform/graphics/multiDrawCommand.h"

namespace visutwin::canvas
{
    class DrawCommands
    {
    public:
        // The 20-byte indexed-indirect record (multiDrawCommand.h).
        using Command = MultiDrawCommand;

        explicit DrawCommands(const int maxCount) : _commands(static_cast<size_t>(std::max(maxCount, 0))) {}

        int maxCount() const { return static_cast<int>(_commands.size()); }

        // The sub-draws update() made live.
        int count() const { return _count; }

        // Writes slot `index` (ignored outside [0, maxCount)).
        void add(const int index, const int count, const int instanceCount, const int first,
            const int baseVertex = 0, const int firstInstance = 0)
        {
            if (index < 0 || index >= maxCount()) {
                return;
            }
            _commands[static_cast<size_t>(index)] = {count, instanceCount, first, baseVertex, firstInstance};
        }

        // Makes the first `count` slots live, clamped to [0, maxCount].
        void update(const int count) { _count = std::clamp(count, 0, maxCount()); }

        const Command& command(const int index) const { return _commands[static_cast<size_t>(index)]; }

        // The primitive of sub-draw `index`: `primitive` narrowed to its range.
        Primitive primitive(const Primitive& primitive, const int index) const
        {
            return narrowedPrimitive(primitive, command(index));
        }

    private:
        std::vector<Command> _commands;
        int _count = 0;
    };
}
