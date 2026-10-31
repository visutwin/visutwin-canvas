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
// DEVIATION: every sub-draw is issued as a draw call of its own, where upstream hands the list to
// one multi-draw call (WebGL's extension, or WebGPU indirect draws). The draws share every bind,
// so what is left per sub-draw is the draw command itself.
//
#pragma once

#include <algorithm>
#include <vector>

#include "mesh.h"

namespace visutwin::canvas
{
    class DrawCommands
    {
    public:
        struct Command
        {
            int count = 0;          // indices (or vertices) to draw
            int instanceCount = 1;
            int first = 0;          // first index (or vertex)
            int baseVertex = 0;     // added to every index
            int firstInstance = 0;
        };

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
            const Command& command = this->command(index);
            Primitive result = primitive;
            result.base = command.first;
            result.count = command.count;
            result.baseVertex = command.baseVertex;
            result.firstInstance = command.firstInstance;
            return result;
        }

    private:
        std::vector<Command> _commands;
        int _count = 0;
    };
}
