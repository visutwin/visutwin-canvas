// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// A container's animation tracks, in the FILE's order,
// with a lookup by name. Not an unordered_map: iterating them, or taking `begin()` as "the"
// animation as several examples do, has to follow the file, not hash order.
//
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "framework/anim/evaluator/animTrack.h"

namespace visutwin::canvas
{
    class AnimTrackList
    {
    public:
        using Entry = std::pair<std::string, std::shared_ptr<AnimTrack>>;
        using const_iterator = std::vector<Entry>::const_iterator;

        /// Append a track; a name already present replaces that entry in place.
        void add(const std::string& name, const std::shared_ptr<AnimTrack>& track)
        {
            const auto it = std::find_if(_entries.begin(), _entries.end(),
                [&](const Entry& entry) { return entry.first == name; });
            if (it != _entries.end()) {
                it->second = track;
            } else {
                _entries.emplace_back(name, track);
            }
        }

        [[nodiscard]] const_iterator find(const std::string& name) const
        {
            return std::find_if(_entries.begin(), _entries.end(),
                [&](const Entry& entry) { return entry.first == name; });
        }
        [[nodiscard]] bool contains(const std::string& name) const { return find(name) != end(); }

        [[nodiscard]] const_iterator begin() const { return _entries.begin(); }
        [[nodiscard]] const_iterator end() const { return _entries.end(); }
        [[nodiscard]] size_t size() const { return _entries.size(); }
        [[nodiscard]] bool empty() const { return _entries.empty(); }
        /// The i-th track in file order.
        [[nodiscard]] const Entry& operator[](const size_t index) const { return _entries[index]; }

    private:
        std::vector<Entry> _entries;
    };
}
