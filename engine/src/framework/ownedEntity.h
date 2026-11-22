// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// An entity an object creates for itself (a gizmo's root, a probe's face cameras, a bake
// camera) and frees when it is done. It usually sits in the scene, where its parent holds it
// in a unique_ptr; deleting it there is still right, because a node deleted while attached
// lets go of its parent's slot first (~GraphNode). So the handle frees with a plain delete,
// attached or not.
//
// The handle also follows the entity's "destroy" event (AGENTS.md, a raw Entity* an object
// keeps): when the scene frees the entity first, say the engine's root goes before the
// object, the handle reads null instead of naming freed memory, and freeing it later does
// nothing. An entity something else only DESTROYS (Entity::destroy, which does not free the
// node) is released by the handle without being freed; its parent still owns it.
//
#pragma once

#include "framework/destroyWatch.h"

namespace visutwin::canvas
{
    class Entity;

    class OwnedEntity
    {
    public:
        OwnedEntity() = default;
        explicit OwnedEntity(Entity* entity);
        ~OwnedEntity();

        OwnedEntity(const OwnedEntity&) = delete;
        OwnedEntity& operator=(const OwnedEntity&) = delete;
        // Moving re-subscribes: the watch's handler names the handle it belongs to.
        OwnedEntity(OwnedEntity&& other) noexcept;
        OwnedEntity& operator=(OwnedEntity&& other) noexcept;

        /// Frees the entity held (if any), then holds `entity`.
        void reset(Entity* entity = nullptr);

        /// Stops holding the entity without freeing it, and returns it.
        [[nodiscard]] Entity* release();

        [[nodiscard]] Entity* get() const { return _entity; }
        Entity* operator->() const { return _entity; }
        Entity& operator*() const { return *_entity; }
        explicit operator bool() const { return _entity != nullptr; }

    private:
        void hold(Entity* entity);

        Entity* _entity = nullptr;
        DestroyWatch _watch;
    };
}
