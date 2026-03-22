// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.11.2025.
//
#include "stringIds.h"

namespace visutwin::canvas
{
    int StringIds::get(const std::string& name) {
        if (const auto it = _map.find(name); it != _map.end()) {
            return it->second;
        }

        const int value = _id++;
        _map[name] = value;
        return value;
    }
}