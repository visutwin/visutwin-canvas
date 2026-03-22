// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.11.2025.
//
#include "utils.h"

namespace visutwin::canvas
{
    uint32_t hash32Fnv1a(const uint32_t* array, size_t length) {
        uint32_t hash = 2166136261u;

        for (size_t i = 0; i < length; i++) {
            const uint32_t prime = 16777619;
            hash ^= array[i];
            hash *= prime;
        }
        return hash;
    }
}