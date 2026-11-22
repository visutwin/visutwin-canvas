// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// OwnedEntity: an entity an object creates, frees with one delete whether or not the scene
// holds it, and forgets when the scene frees it first. "Freed" is read from the entity
// index (Entity::findById answers null for a freed entity), never from the freed memory.
//
#include <memory>
#include <utility>
#include <vector>

#include "framework/entity.h"
#include "framework/ownedEntity.h"
#include "support/check.h"

using visutwin::canvas::Entity;
using visutwin::canvas::EntityId;
using visutwin::canvas::OwnedEntity;
using visutwin::canvas::test::check;
using visutwin::canvas::test::finish;

namespace
{
    bool alive(const EntityId id) { return Entity::findById(id) != nullptr; }
}

int main()
{
    // Detached: the handle is the only owner.
    {
        EntityId id;
        {
            OwnedEntity owned(new Entity());
            id = owned->id();
            check(alive(id), "a held detached entity is alive");
        }
        check(!alive(id), "the handle frees a detached entity when it goes");
    }

    // Attached: the parent's slot is let go and the entity freed once.
    {
        auto root = std::make_unique<Entity>();
        OwnedEntity owned(new Entity());
        root->addChild(owned.get());
        const EntityId id = owned->id();
        check(root->children().size() == 1, "the scene holds the entity");
        owned.reset();
        check(!owned && !alive(id), "reset frees an attached entity");
        check(root->children().empty(), "and its parent no longer lists it");
    }

    // The scene frees the entity first: the handle forgets it and frees nothing later.
    {
        OwnedEntity owned;
        EntityId id;
        {
            auto root = std::make_unique<Entity>();
            owned.reset(new Entity());
            root->addChild(owned.get());
            id = owned->id();
        }
        check(!alive(id), "freeing the parent frees the entity");
        check(!owned, "and the handle reads null instead of naming freed memory");
        owned.reset();   // under the sanitizers a dangling handle fails here
    }

    // Destroyed but not freed: the parent still owns it, the handle lets go.
    {
        auto root = std::make_unique<Entity>();
        OwnedEntity owned(new Entity());
        Entity* entity = owned.get();
        root->addChild(entity);
        entity->destroy();
        check(!owned, "a destroyed entity is let go");
        // findById hides a destroying() entity, so the parent's list is the witness here.
        check(root->children().size() == 1 && root->children()[0].get() == entity,
            "and stays with the parent that owns it");
    }

    // Moving keeps one owner, and the moved-to handle follows the entity.
    {
        std::vector<OwnedEntity> handles;
        auto root = std::make_unique<Entity>();
        for (int i = 0; i < 8; ++i) {   // grows the vector, moving the handles
            handles.emplace_back(new Entity());
            root->addChild(handles.back().get());
        }
        const EntityId first = handles.front()->id();
        OwnedEntity moved = std::move(handles.front());
        check(!handles.front() && moved && moved->id() == first, "a move hands the entity over");
        root.reset();
        check(!moved, "the moved-to handle follows the entity's destroy");
        bool allNull = true;
        for (const auto& handle : handles) {
            allNull = allNull && !handle;
        }
        check(allNull, "every handle moved by the vector's growth follows its entity too");
    }

    // Release hands the entity back without freeing it.
    {
        OwnedEntity owned(new Entity());
        const EntityId id = owned->id();
        std::unique_ptr<Entity> back(owned.release());
        check(!owned && alive(id), "release lets go without freeing");
    }

    return finish("owned-entity");
}
