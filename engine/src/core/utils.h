// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.11.2025.
//
#pragma once

#include <bitset>

namespace visutwin::canvas
{
    using BitPacking = std::bitset<32>;

    /**
     * Calculates a simple 32bit hash value of an array of 32bit integer numbers. Designed for
     * performance but provides good distribution with a small number of collisions. Based on
     * FNV-1a non-cryptographic hash function.
     */
    uint32_t hash32Fnv1a(const uint32_t* array, size_t length);
}
