// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "defaultAnimBinder.h"

#include "framework/entity.h"

namespace visutwin::canvas
{
    DefaultAnimBinder::DefaultAnimBinder(Entity* entity) : _entity(entity)
    {
    }

    GraphNode* DefaultAnimBinder::resolve(const std::string& path)
    {
        const auto it = _nodes.find(path);
        if (it != _nodes.end()) {
            return it->second;
        }

        if (!_entity) {
            return nullptr;
        }

        GraphNode* node = _entity->findByName(path);
        _nodes[path] = node;
        return node;
    }

    void DefaultAnimBinder::unresolve(const std::string& path)
    {
        _nodes.erase(path);
    }
}
