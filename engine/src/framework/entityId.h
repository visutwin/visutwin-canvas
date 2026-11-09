// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
// The name an entity keeps for its whole life, for code that must not hold its pointer: a
// frontend in another language, a saved selection, a message naming an entity. Ids are handed
// out by one process-wide counter and never reused, so an id kept after its entity is gone
// finds nothing (Engine::entityById) rather than whatever entity the address now holds.
//
#pragma once

#include <cstdint>
#include <functional>

namespace visutwin::canvas
{
    struct EntityId
    {
        /// 0 names no entity; every entity's id is larger.
        uint64_t value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const EntityId&) const = default;
    };
}

template <>
struct std::hash<visutwin::canvas::EntityId>
{
    size_t operator()(const visutwin::canvas::EntityId id) const noexcept { return std::hash<uint64_t>{}(id.value); }
};
