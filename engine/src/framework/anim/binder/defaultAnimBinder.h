// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <string>
#include <unordered_map>

#include "animBinder.h"

namespace visutwin::canvas
{
    class Entity;
    class GraphNode;

    class DefaultAnimBinder : public AnimBinder
    {
    public:
        explicit DefaultAnimBinder(Entity* entity);

        GraphNode* resolve(const std::string& path) override;
        void unresolve(const std::string& path) override;

    private:
        Entity* _entity = nullptr;
        std::unordered_map<std::string, GraphNode*> _nodes;
    };
}
