// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.11.2025.
//
#pragma once

#include <string>
#include <unordered_map>

namespace visutwin::canvas
{
    /**
     * A cache for assigning unique numerical ids to strings
     */
    class StringIds
    {
    public:
        /**
         * Get the id for the given name. If the name has not been seen before, it will be assigned a new id.
         */
        int get(const std::string& name);

    private:
        std::unordered_map<std::string, int> _map;

        int _id = 0;
    };
}
