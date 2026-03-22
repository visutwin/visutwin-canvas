// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.10.2025.
//
#pragma once
#include <initializer_list>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "eventHandler.h"

namespace visutwin::canvas
{
    /**
     * Tags is a powerful tag management system for categorizing and filtering objects in VisuTwin
     * applications. It provides an efficient way to attach string identifiers to objects and query them
     * using logical operations.
     *
     * Tags are automatically available on Assets and Entities (see Asset::tags and GraphNode::tags).
     * You can search for specific assets via AssetRegistry::findByTag and specific entities via
     * GraphNode::findByTag.
     */
    class Tags : public EventHandler
    {
    public:
        static constexpr const char* EVENT_ADD = "add";
        static constexpr const char* EVENT_REMOVE = "remove";
        static constexpr const char* EVENT_CHANGE = "change";

        explicit Tags(void* parent = nullptr): _parent(parent) {}

        bool add(const std::string& tag);
        bool add(const std::vector<std::string>& tags);
        bool add(std::initializer_list<std::string> tags);
        bool add(const char* tag);

        template<typename... Args>
        bool add(Args&&... args)
        {
            std::vector<std::string> tags;
            tags.reserve(sizeof...(Args));
            (appendFlat(tags, std::forward<Args>(args)), ...);
            return add(tags);
        }

        bool remove(const std::string& tag);
        bool remove(const std::vector<std::string>& tags);
        bool remove(std::initializer_list<std::string> tags);
        bool remove(const char* tag);

        template<typename... Args>
        bool remove(Args&&... args)
        {
            std::vector<std::string> tags;
            tags.reserve(sizeof...(Args));
            (appendFlat(tags, std::forward<Args>(args)), ...);
            return remove(tags);
        }

        void clear();

        [[nodiscard]] bool has(const std::string& tag) const;
        [[nodiscard]] bool has(const std::vector<std::string>& tags) const;
        [[nodiscard]] bool has(std::initializer_list<std::string> tags) const;
        [[nodiscard]] bool has(const char* tag) const;
        [[nodiscard]] bool has(const std::vector<std::vector<std::string>>& query) const;

        template<typename... Args>
        [[nodiscard]] bool has(Args&&... args) const
        {
            std::vector<std::vector<std::string>> query;
            query.reserve(sizeof...(Args));
            (appendQuery(query, std::forward<Args>(args)), ...);
            return hasInternal(query);
        }

        [[nodiscard]] std::vector<std::string> list() const { return _list; }
        [[nodiscard]] size_t size() const { return _list.size(); }
        [[nodiscard]] bool empty() const { return _list.empty(); }

    private:
        static void appendFlat(std::vector<std::string>& out, const std::string& tag);
        static void appendFlat(std::vector<std::string>& out, const char* tag);
        static void appendFlat(std::vector<std::string>& out, const std::vector<std::string>& tags);
        static void appendFlat(std::vector<std::string>& out, std::initializer_list<std::string> tags);

        static void appendQuery(std::vector<std::vector<std::string>>& out, const std::string& tag);
        static void appendQuery(std::vector<std::vector<std::string>>& out, const char* tag);
        static void appendQuery(std::vector<std::vector<std::string>>& out, const std::vector<std::string>& tags);
        static void appendQuery(std::vector<std::vector<std::string>>& out, std::initializer_list<std::string> tags);

        [[nodiscard]] bool hasInternal(const std::vector<std::vector<std::string>>& query) const;

        // Parent object who tags belong to
        void* _parent = nullptr;

        // Index for fast lookup.
        std::unordered_set<std::string> _index;

        // Stable order of insertion.
        std::vector<std::string> _list;
    };
}
