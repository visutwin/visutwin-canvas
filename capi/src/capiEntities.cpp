// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// C API: entities and their components.
#include "capiInternal.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::capi;

extern "C" {

// ---- Entities -------------------------------------------------------------------------

vt_status vt_entity_root(vt_engine* engine, vt_entity* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *out = engine->engine->root()->id().value;
        return VT_OK;
    });
}

vt_status vt_entity_create(vt_engine* engine, const vt_entity parent, const char* name, vt_entity* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        Entity* parentEntity = engine->engine->root();
        if (parent != 0) {
            if (const vt_status status = findEntity(engine, parent, parentEntity); status != VT_OK) {
                return status;
            }
        }
        auto entity = std::make_unique<Entity>();
        entity->setEngine(engine->engine.get());
        entity->setName(name ? name : "");
        Entity* created = entity.get();
        parentEntity->addChild(std::move(entity));
        if (out) {
            *out = created->id().value;
        }
        return VT_OK;
    });
}

vt_status vt_entity_destroy(vt_engine* engine, const vt_entity id)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        if (entity == engine->engine->root() || !entity->parent()) {
            return fail(VT_ERROR_INVALID_STATE, "the root cannot be destroyed");
        }
        // destroy() releases every component through its system, which clears its state here.
        entity->destroy();
        // The parent owned it; taking it back frees it and everything below it.
        std::unique_ptr<GraphNode> owned = entity->parent()->removeChild(entity);
        owned.reset();
        return VT_OK;
    });
}

vt_status vt_entity_alive(vt_engine* engine, const vt_entity id, int32_t* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *out = engine->engine->entityById(EntityId{id}) ? 1 : 0;
        return VT_OK;
    });
}

vt_status vt_entity_find(vt_engine* engine, const char* path, vt_entity* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!path || !out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no path or no output");
        }
        Entity* entity = engine->engine->entityByPath(path);
        if (!entity) {
            return fail(VT_ERROR_NOT_FOUND, std::string("no entity at '") + path + "'");
        }
        *out = entity->id().value;
        return VT_OK;
    });
}

vt_status vt_entity_path(vt_engine* engine, const vt_entity id, char* buffer, const size_t capacity, size_t* length)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        return writeString(entity->path(), buffer, capacity, length);
    });
}

vt_status vt_entity_parent(vt_engine* engine, const vt_entity id, vt_entity* out)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        const auto* parent = dynamic_cast<const Entity*>(entity->parent());
        *out = parent ? parent->id().value : 0;
        return VT_OK;
    });
}

vt_status vt_entity_set_parent(vt_engine* engine, const vt_entity id, const vt_entity parent)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        Entity* newParent = engine->engine->root();
        if (parent != 0) {
            if (const vt_status status = findEntity(engine, parent, newParent); status != VT_OK) {
                return status;
            }
        }
        if (!entity->parent()) {
            return fail(VT_ERROR_INVALID_STATE, "the root has no parent to change");
        }
        for (const GraphNode* node = newParent; node; node = node->parent()) {
            if (node == entity) {
                return fail(VT_ERROR_INVALID_ARGUMENT, "an entity cannot move below itself");
            }
        }
        if (entity->parent() != newParent) {
            newParent->addChild(static_cast<GraphNode*>(entity));
        }
        return VT_OK;
    });
}

vt_status vt_entity_children(vt_engine* engine, const vt_entity id, vt_entity* buffer, const size_t capacity,
    size_t* count)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        std::vector<vt_entity> children;
        entity->forEachChild([&](GraphNode* child) {
            if (const auto* childEntity = dynamic_cast<const Entity*>(child)) {
                children.push_back(childEntity->id().value);
            }
        });
        if (count) {
            *count = children.size();
        }
        if (!buffer) {
            return VT_OK;
        }
        const size_t written = std::min(capacity, children.size());
        std::memcpy(buffer, children.data(), written * sizeof(vt_entity));
        if (written < children.size()) {
            return fail(VT_ERROR_BUFFER_TOO_SMALL, "the entity has " + std::to_string(children.size()) + " children");
        }
        return VT_OK;
    });
}

vt_status vt_entity_describe(vt_engine* engine, const vt_entity id, char* buffer, const size_t capacity,
    size_t* length)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        return writeString(describeEntityProperties(*entity), buffer, capacity, length);
    });
}

// ---- Components -----------------------------------------------------------------------

vt_status vt_component_add(vt_engine* engine, const vt_entity id, const char* typeName)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        vt_status status = VT_OK;
        const TypeDesc* type = componentType(typeName, status);
        if (!type) {
            return status;
        }
        if (!type->addTo(*entity)) {
            return fail(VT_ERROR_UNSUPPORTED, "entity " + std::to_string(id) + " cannot take a " + type->name +
                (type->name == "cameraControls" ? " (it needs a camera)" : ""));
        }
        return VT_OK;
    });
}

vt_status vt_component_remove(vt_engine* engine, const vt_entity id, const char* typeName)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        vt_status status = VT_OK;
        const TypeDesc* type = componentType(typeName, status);
        if (!type) {
            return status;
        }
        if (!type->removeFrom(*entity)) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(id) + " has no " + type->name);
        }
        return VT_OK;
    });
}

vt_status vt_component_has(vt_engine* engine, const vt_entity id, const char* typeName, int32_t* out)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        vt_status status = VT_OK;
        const TypeDesc* type = componentType(typeName, status);
        if (!type) {
            return status;
        }
        *out = type->resolve(*entity) ? 1 : 0;
        return VT_OK;
    });
}

} // extern "C"
