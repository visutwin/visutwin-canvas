// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.07.2026.
//
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace visutwin::canvas
{
    /**
     * @brief Registry of named Metal shader chunks with user overrides (port of
     * upstream `ShaderChunks`).
     * @ingroup group_scene_shaderlib
     *
     * Holds the engine's default chunk sources (loaded once per process from
     * `engine/shaders/metal/chunks`, one file per chunk, keyed by file stem) plus a
     * per-instance override map. `get()` resolves override-over-default. `hash()`
     * fingerprints the override set and is folded into shader variant cache keys, so
     * changing a chunk at runtime invalidates affected cached programs instead of
     * silently reusing stale binaries (upstream cache-invalidation hashing).
     *
     * One instance lives on each ProgramLibrary — i.e. per graphics device, matching
     * upstream's per-device DeviceCache of ShaderChunks. Per-material overrides layer
     * on top via `Material::setShaderChunk` and are resolved at composition time.
     *
     * DEVIATION: upstream registers ~250 GLSL/WGSL micro-chunks; this port composes
     * Metal source from a smaller set of ordered chunk files, since MSL variants are
     * compiled as one translation unit rather than a fine-grained function library.
     */
    class ShaderChunks
    {
    public:
        ShaderChunks();

        /// True when the default chunk sources were found on disk.
        bool loaded() const { return _defaults != nullptr; }

        /// Directory the default chunks were loaded from.
        const std::filesystem::path& rootPath() const;

        /// Effective source for a chunk: override when present, default otherwise.
        /// Returns nullptr for unknown names.
        const std::string* get(const std::string& name) const;

        /// True when a default or override exists for the name.
        bool has(const std::string& name) const;

        /// Override (or add) a chunk source. Overriding recomputes the registry hash;
        /// shader variants recompile lazily under their new cache keys.
        void set(const std::string& name, std::string source);

        /// Remove an override, restoring the default source. Returns false when the
        /// name was not overridden.
        bool remove(const std::string& name);

        /// Drop all overrides.
        void clearOverrides();

        size_t overrideCount() const { return _overrides.size(); }
        const std::unordered_map<std::string, std::string>& overrides() const { return _overrides; }

        /// Fingerprint of the override set (0 when no overrides). Folded into shader
        /// variant cache keys for invalidation.
        uint64_t hash() const { return _hash; }

        /// Names of all default chunks.
        std::vector<std::string> names() const;

        /// FNV-1a helper shared with per-material override hashing.
        static uint64_t hashChunkMap(const std::unordered_map<std::string, std::string>& chunks);

    private:
        const std::unordered_map<std::string, std::string>* _defaults = nullptr;
        std::unordered_map<std::string, std::string> _overrides;
        uint64_t _hash = 0;
    };
}
