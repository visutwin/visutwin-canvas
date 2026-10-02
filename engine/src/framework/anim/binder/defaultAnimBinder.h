// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
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
        std::vector<MorphInstance*> resolveMorphInstances(const std::string& path) override;
        uint64_t version() const override { return _version; }

    private:
        uint64_t _version = 0;
        Entity* _entity = nullptr;
        std::unordered_map<std::string, GraphNode*> _nodes;
        std::unordered_map<std::string, std::vector<MorphInstance*>> _morphInstances;
    };
}
