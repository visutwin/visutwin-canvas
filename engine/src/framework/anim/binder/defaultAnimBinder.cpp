// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "defaultAnimBinder.h"

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
