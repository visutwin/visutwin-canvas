// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.07.2026
//
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    /**
     * @brief Registry of the forward program's shader chunks, with user overrides.
     * @ingroup group_scene_shaderlib
     *
     * The defaults are the Slang chunks under `engine/shaders/slang/forward` (one file per
     * chunk, keyed by file stem; `programs/forward.slang` and `programs/shadow.slang`
     * #include them), read once per process. An override replaces that FILE wherever a
     * program includes it, as Slang source, and the program is then compiled at run time
     * (`SlangCompileRequest::virtualFiles`); a name no program includes changes nothing and
     * is reported. `get()` resolves override-over-default, and `hash()` fingerprints the
     * override set, folded into the variant cache key so a change compiles a new variant.
     *
     * One instance lives on each ProgramLibrary, i.e. per graphics device. Per-material
     * overrides layer on top through `Material::setShaderChunk`; a material's wins.
     */
    /// The directories the shader source tree (`engine/shaders/slang/...`) is looked for
    /// under, in order: $VISUTWIN_CANVAS_SHADERS, the engine's own source tree, the working
    /// directory and two parents, and the installed data directory. Shared by the chunk
    /// registry and the Slang runtime compile.
    std::vector<std::filesystem::path> shaderSourceRoots();

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

        /// Names of all default chunks, sorted.
        std::vector<std::string> names() const;

        /// FNV-1a helper shared with per-material override hashing.
        static uint64_t hashChunkMap(const std::unordered_map<std::string, std::string>& chunks);

    private:
        const std::unordered_map<std::string, std::string>* _defaults = nullptr;
        std::unordered_map<std::string, std::string> _overrides;
        uint64_t _hash = 0;
    };
}
