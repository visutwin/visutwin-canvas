// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "defaultAnimBinder.h"

#include <spdlog/spdlog.h>
#include <string>
#include <vector>

#include "framework/entity.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "scene/meshInstance.h"
#include "scene/morphInstance.h"

namespace visutwin::canvas
{
    DefaultAnimBinder::DefaultAnimBinder(Entity* entity) : _entity(entity)
    {
    }

    namespace
    {
        std::vector<std::string> splitPath(const std::string& path)
        {
            std::vector<std::string> parts;
            size_t start = 0;
            while (true) {
                const size_t slash = path.find('/', start);
                if (slash == std::string::npos) {
                    parts.push_back(path.substr(start));
                    return parts;
                }
                parts.push_back(path.substr(start, slash - start));
                start = slash + 1;
            }
        }

        // Follow parts[start..] down through direct children by name, upstream's
        // GraphNode.findByPath. Null as soon as one segment has no such child.
        GraphNode* walkPath(GraphNode* from, const std::vector<std::string>& parts, const size_t start)
        {
            GraphNode* current = from;
            for (size_t i = start; i < parts.size() && current; ++i) {
                GraphNode* next = nullptr;
                for (const auto& child : current->children()) {
                    if (child->name() == parts[i]) {
                        next = child.get();
                        break;
                    }
                }
                current = next;
            }
            return current;
        }
    }

    // A target is a PATH of node names from the model's root down ("Root/Arm/Hand",
    // what the GLB parser writes) or a bare name (anything hand-authored). A path
    // is walked, not searched, so two nodes sharing a name in different branches
    // resolve to their own entity — upstream's DefaultAnimBinder does the same
    // (findByPath, then findByPath from the root's own children, then the leaf
    // name). The bound entity may be the model's root itself (a single-root scene
    // instantiates as its root), a wrapper holding the roots, or an app entity the
    // model was parented under at any depth, so the walk is tried from each.
    GraphNode* DefaultAnimBinder::resolve(const std::string& path)
    {
        const auto it = _nodes.find(path);
        if (it != _nodes.end()) {
            return it->second;
        }

        if (!_entity) {
            return nullptr;
        }

        GraphNode* node = nullptr;
        const std::vector<std::string> parts = splitPath(path);
        if (parts.size() > 1) {
            // The path's root is a child of the bound entity (a wrapper, or the app's
            // own entity holding the model).
            node = walkPath(_entity, parts, 0);
            // The bound entity IS the path's root.
            if (!node && _entity->name() == parts.front()) {
                node = walkPath(_entity, parts, 1);
            }
            // The path's root sits deeper under the bound entity.
            if (!node) {
                if (GraphNode* anchor = _entity->findByName(parts.front())) {
                    node = walkPath(anchor, parts, 1);
                }
            }
            // Last resort, the leaf name alone: the model was re-parented in a way
            // no walk covers, or its nodes were renamed. Ambiguous under duplicate
            // names, which is the case the path exists to avoid.
            if (!node) {
                node = _entity->findByName(parts.back());
                if (node) {
                    spdlog::debug("DefaultAnimBinder: '{}' not found as a path under '{}', bound to the first '{}'",
                        path, _entity->name(), parts.back());
                }
            }
        } else {
            node = _entity->findByName(path);
        }
        _nodes[path] = node;
        return node;
    }

    void DefaultAnimBinder::unresolve(const std::string& path)
    {
        _nodes.erase(path);
        _morphInstances.erase(path);
    }

    std::vector<MorphInstance*> DefaultAnimBinder::resolveMorphInstances(const std::string& path)
    {
        const auto it = _morphInstances.find(path);
        if (it != _morphInstances.end()) {
            return it->second;
        }

        std::vector<MorphInstance*> result;
        // glTF "weights" channels target the MESH node — the entity carrying the
        // RenderComponent whose mesh instances own the morph instances.
        if (auto* node = resolve(path)) {
            if (auto* entity = dynamic_cast<Entity*>(node)) {
                if (auto* render = entity->findComponent<RenderComponent>()) {
                    for (auto* meshInstance : render->meshInstances()) {
                        if (meshInstance && meshInstance->morphInstance()) {
                            result.push_back(meshInstance->morphInstance());
                        }
                    }
                }
            }
        }
        _morphInstances[path] = result;
        return result;
    }
}
