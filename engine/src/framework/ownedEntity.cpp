// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
#include "ownedEntity.h"

#include <utility>

#include "framework/entity.h"

namespace visutwin::canvas
{
    OwnedEntity::OwnedEntity(Entity* entity)
    {
        hold(entity);
    }

    OwnedEntity::~OwnedEntity()
    {
        reset();
    }

    OwnedEntity::OwnedEntity(OwnedEntity&& other) noexcept
    {
        hold(other.release());
    }

    OwnedEntity& OwnedEntity::operator=(OwnedEntity&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    void OwnedEntity::reset(Entity* entity)
    {
        // Let go before freeing: the destroy event the delete fires must find nothing to clear.
        delete release();
        hold(entity);
    }

    Entity* OwnedEntity::release()
    {
        _watch.reset();
        return std::exchange(_entity, nullptr);
    }

    void OwnedEntity::hold(Entity* entity)
    {
        _entity = entity;
        if (entity) {
            _watch.watch(entity, [this] { _entity = nullptr; });
        }
    }
}
