// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.10.2025.
//
#include "tags.h"
#include <algorithm>

namespace visutwin::canvas
{
    void Tags::appendFlat(std::vector<std::string>& out, const std::string& tag)
    {
        if (!tag.empty()) {
            out.push_back(tag);
        }
    }

    void Tags::appendFlat(std::vector<std::string>& out, const char* tag)
    {
        if (tag && *tag) {
            out.emplace_back(tag);
        }
    }

    void Tags::appendFlat(std::vector<std::string>& out, const std::vector<std::string>& tags)
    {
        for (const auto& tag : tags) {
            appendFlat(out, tag);
        }
    }

    void Tags::appendFlat(std::vector<std::string>& out, const std::initializer_list<std::string> tags)
    {
        for (const auto& tag : tags) {
            appendFlat(out, tag);
        }
    }

    void Tags::appendQuery(std::vector<std::vector<std::string>>& out, const std::string& tag)
    {
        if (!tag.empty()) {
            out.push_back({tag});
        }
    }

    void Tags::appendQuery(std::vector<std::vector<std::string>>& out, const char* tag)
    {
        if (tag && *tag) {
            out.push_back({std::string(tag)});
        }
    }

    void Tags::appendQuery(std::vector<std::vector<std::string>>& out, const std::vector<std::string>& tags)
    {
        std::vector<std::string> group;
        group.reserve(tags.size());
        for (const auto& tag : tags) {
            if (!tag.empty()) {
                group.push_back(tag);
            }
        }
        if (!group.empty()) {
            out.push_back(std::move(group));
        }
    }

    void Tags::appendQuery(std::vector<std::vector<std::string>>& out, const std::initializer_list<std::string> tags)
    {
        std::vector<std::string> group;
        group.reserve(tags.size());
        for (const auto& tag : tags) {
            if (!tag.empty()) {
                group.push_back(tag);
            }
        }
        if (!group.empty()) {
            out.push_back(std::move(group));
        }
    }

    bool Tags::add(const std::string& tag)
    {
        if (tag.empty()) {
            return false;
        }

        if (_index.contains(tag)) {
            return false;
        }

        _index.insert(tag);
        _list.push_back(tag);

        fire(EVENT_ADD, tag, _parent);
        fire(EVENT_CHANGE, _parent);
        return true;
    }

    bool Tags::add(const std::vector<std::string>& tags)
    {
        bool changed = false;
        for (const auto& tag : tags) {
            if (tag.empty()) {
                continue;
            }

            if (_index.contains(tag)) {
                continue;
            }

            changed = true;
            _index.insert(tag);
            _list.push_back(tag);
            fire(EVENT_ADD, tag, _parent);
        }

        if (changed) {
            fire(EVENT_CHANGE, _parent);
        }

        return changed;
    }

    bool Tags::add(std::initializer_list<std::string> tags)
    {
        return add(std::vector<std::string>(tags));
    }

    bool Tags::add(const char* tag)
    {
        return add(std::string(tag ? tag : ""));
    }

    bool Tags::remove(const std::string& tag)
    {
        if (tag.empty()) {
            return false;
        }

        if (!_index.contains(tag)) {
            return false;
        }

        _index.erase(tag);
        _list.erase(std::remove(_list.begin(), _list.end(), tag), _list.end());

        fire(EVENT_REMOVE, tag, _parent);
        fire(EVENT_CHANGE, _parent);
        return true;
    }

    bool Tags::remove(const std::vector<std::string>& tags)
    {
        bool changed = false;
        for (const auto& tag : tags) {
            if (tag.empty()) {
                continue;
            }

            if (!_index.contains(tag)) {
                continue;
            }

            changed = true;
            _index.erase(tag);
            _list.erase(std::remove(_list.begin(), _list.end(), tag), _list.end());
            fire(EVENT_REMOVE, tag, _parent);
        }

        if (changed) {
            fire(EVENT_CHANGE, _parent);
        }

        return changed;
    }

    bool Tags::remove(std::initializer_list<std::string> tags)
    {
        return remove(std::vector<std::string>(tags));
    }

    bool Tags::remove(const char* tag)
    {
        return remove(std::string(tag ? tag : ""));
    }

    void Tags::clear()
    {
        if (_list.empty()) {
            return;
        }

        auto tagsCopy = _list;
        _list.clear();
        _index.clear();

        for (const std::string& tag : tagsCopy) {
            fire(EVENT_REMOVE, tag, _parent);
        }
        fire(EVENT_CHANGE, _parent);
    }

    bool Tags::has(const std::string& tag) const
    {
        return !tag.empty() && _index.contains(tag);
    }

    bool Tags::has(const std::vector<std::string>& tags) const
    {
        return hasInternal({tags});
    }

    bool Tags::has(std::initializer_list<std::string> tags) const
    {
        return has(std::vector<std::string>(tags));
    }

    bool Tags::has(const char* tag) const
    {
        return has(std::string(tag ? tag : ""));
    }

    bool Tags::has(const std::vector<std::vector<std::string>>& query) const
    {
        return hasInternal(query);
    }

    bool Tags::hasInternal(const std::vector<std::vector<std::string>>& query) const
    {
        if (_list.empty() || query.empty()) {
            return false;
        }

        for (const auto& group : query) {
            if (group.size() == 1) {
                if (_index.contains(group[0])) {
                    return true;
                }
                continue;
            }

            bool allPresent = true;
            for (const std::string& tag : group) {
                if (!_index.contains(tag)) {
                    allPresent = false;
                    break;
                }
            }

            if (allPresent) {
                return true;
            }
        }

        return false;
    }
}
