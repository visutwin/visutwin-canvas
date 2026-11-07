// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.11.2026
//
#include "bindlessResidency.h"

#include <atomic>

namespace visutwin::canvas
{
    namespace
    {
        struct Registry
        {
            std::mutex mutex;
            std::vector<BindlessResidency::Entry> entries{1};   // entry 0: "no map"
            std::vector<uint32_t> freeList;
            uint64_t nextVersion = 1;
            std::atomic<uint64_t> generation{1};
        };

        Registry& registry()
        {
            static Registry instance;
            return instance;
        }

        struct SamplerRegistry
        {
            std::mutex mutex;
            std::vector<TextureSamplerState> states;
            std::atomic<uint64_t> generation{1};
        };

        SamplerRegistry& samplerRegistry()
        {
            static SamplerRegistry instance;
            if (instance.states.empty()) {
                instance.states.push_back(BindlessSamplers::defaultState());
            }
            return instance;
        }
    }

    uint32_t BindlessResidency::acquire(Texture* texture)
    {
        Registry& r = registry();
        std::lock_guard lock(r.mutex);
        uint32_t index;
        if (!r.freeList.empty()) {
            index = r.freeList.back();
            r.freeList.pop_back();
        } else {
            index = static_cast<uint32_t>(r.entries.size());
            r.entries.emplace_back();
        }
        r.entries[index] = {texture, r.nextVersion++};
        r.generation.fetch_add(1, std::memory_order_relaxed);
        return index;
    }

    void BindlessResidency::release(const uint32_t index)
    {
        if (index == kNone) {
            return;
        }
        Registry& r = registry();
        std::lock_guard lock(r.mutex);
        if (index >= r.entries.size() || r.entries[index].texture == nullptr) {
            return;
        }
        r.entries[index] = {nullptr, r.nextVersion++};
        r.freeList.push_back(index);
        r.generation.fetch_add(1, std::memory_order_relaxed);
    }

    void BindlessResidency::touch(const uint32_t index)
    {
        if (index == kNone) {
            return;
        }
        Registry& r = registry();
        std::lock_guard lock(r.mutex);
        if (index < r.entries.size()) {
            r.entries[index].version = r.nextVersion++;
            r.generation.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void BindlessResidency::snapshot(std::vector<Entry>& out)
    {
        Registry& r = registry();
        std::lock_guard lock(r.mutex);
        out = r.entries;
    }

    uint64_t BindlessResidency::generation()
    {
        return registry().generation.load(std::memory_order_relaxed);
    }

    TextureSamplerState BindlessSamplers::defaultState()
    {
        return defaultTextureSamplerState(kAnisotropy);
    }

    uint32_t BindlessSamplers::indexOf(const TextureSamplerState& state)
    {
        SamplerRegistry& r = samplerRegistry();
        std::lock_guard lock(r.mutex);
        for (size_t i = 0; i < r.states.size(); ++i) {
            if (r.states[i] == state) {
                return static_cast<uint32_t>(i);
            }
        }
        r.states.push_back(state);
        r.generation.fetch_add(1, std::memory_order_relaxed);
        return static_cast<uint32_t>(r.states.size() - 1);
    }

    void BindlessSamplers::snapshot(std::vector<TextureSamplerState>& out)
    {
        SamplerRegistry& r = samplerRegistry();
        std::lock_guard lock(r.mutex);
        out = r.states;
    }

    uint64_t BindlessSamplers::generation()
    {
        return samplerRegistry().generation.load(std::memory_order_relaxed);
    }
}
