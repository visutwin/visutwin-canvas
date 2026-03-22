// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <string>

namespace visutwin::canvas
{
    class GraphNode;

    class AnimBinder
    {
    public:
        virtual ~AnimBinder() = default;

        virtual GraphNode* resolve(const std::string& path) = 0;
        virtual void unresolve(const std::string& path) = 0;
    };
}
