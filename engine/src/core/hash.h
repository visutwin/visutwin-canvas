// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// FNV-1a, 64-bit: over bytes (text, raw memory) or folding whole 64-bit words, with the
// standard offset basis. One definition, so every hash in the engine starts from the same
// basis and nothing re-spells the constants: the shader disk cache's on-disk keys come
// from fnv1a64 over text, so this must stay byte-for-byte the standard FNV-1a.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace visutwin::canvas
{
    inline constexpr uint64_t kFnv1aOffsetBasis = 14695981039346656037ull;
    inline constexpr uint64_t kFnv1aPrime = 1099511628211ull;

    /// One byte into a running FNV-1a hash.
    constexpr uint64_t fnv1aByte(const uint64_t hash, const uint8_t byte)
    {
        return (hash ^ byte) * kFnv1aPrime;
    }

    /// FNV-1a over the bytes of `text`, continuing from `hash` (the offset basis to start).
    constexpr uint64_t fnv1a64(const std::string_view text, uint64_t hash = kFnv1aOffsetBasis)
    {
        for (const char c : text) {
            hash = fnv1aByte(hash, static_cast<uint8_t>(c));
        }
        return hash;
    }

    /// FNV-1a over the little-endian bytes of `value`, continuing from `hash`.
    constexpr uint64_t fnv1aBytesOf(uint64_t hash, uint64_t value, const int byteCount = 8)
    {
        for (int i = 0; i < byteCount; ++i) {
            hash = fnv1aByte(hash, static_cast<uint8_t>(value & 0xffu));
            value >>= 8;
        }
        return hash;
    }

    /// Folds a whole 64-bit word into a running hash in one FNV-1a step: the cheap form
    /// for combining values that are already hashes, keys or small integers.
    constexpr uint64_t fnv1aMix(const uint64_t hash, const uint64_t word)
    {
        return (hash ^ word) * kFnv1aPrime;
    }
}
