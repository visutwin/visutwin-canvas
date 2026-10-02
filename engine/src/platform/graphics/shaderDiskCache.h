// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026
//
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace visutwin::canvas
{
    /**
     * Compiled shader products kept on disk between runs, for a backend whose own
     * system does not keep them: SPIR-V the Vulkan backend compiles from GLSL at run
     * time, and its serialized pipeline cache. (Metal needs none: the system caches
     * every library and pipeline it compiles, keyed on the source.)
     *
     * An entry is FOUND by a hash of its key and ACCEPTED only when the key stored
     * beside the payload is the caller's key, byte for byte — the key is the whole of
     * what the payload was made from (the source text included), so a hash collision
     * or a stale file can never hand back another shader's code. A file that is short,
     * damaged or from another format version is a miss.
     *
     * No GPU types: tests/shaderDiskCacheTests.cpp holds the format and the misses.
     */
    class ShaderDiskCache
    {
    public:
        /// A cache that stores and finds nothing.
        ShaderDiskCache() = default;
        /// A cache in `directory`, created on the first store. Empty = disabled.
        explicit ShaderDiskCache(std::filesystem::path directory);

        [[nodiscard]] bool enabled() const { return !_directory.empty(); }
        [[nodiscard]] const std::filesystem::path& directory() const { return _directory; }

        /// The payload stored under (`kind`, `key`), or nothing. `kind` names what the
        /// payload is ("spirv", "pipelines") and is part of the file's name.
        [[nodiscard]] std::optional<std::vector<uint8_t>> load(std::string_view kind, std::string_view key) const;

        /// Stores `payload`, replacing any entry of that name, through a temporary file
        /// and a rename so a reader never sees half of it. False if it could not.
        bool store(std::string_view kind, std::string_view key, std::span<const uint8_t> payload) const;

        /**
         * The directory a device should cache in. `configured` is the application's
         * choice (GraphicsDeviceOptions::shaderCacheDirectory); empty means the
         * per-user default: ~/Library/Caches/visutwin-canvas on macOS,
         * $XDG_CACHE_HOME/visutwin-canvas (or ~/.cache/...) elsewhere, %LOCALAPPDATA%
         * on Windows. The environment overrides both: VISUTWIN_SHADER_CACHE_DIR names a
         * directory, VISUTWIN_SHADER_CACHE=0 turns caching off. Returns an empty path
         * when caching is off or no location can be found.
         */
        static std::filesystem::path resolveDirectory(const std::string& configured, bool enabled);

    private:
        [[nodiscard]] std::filesystem::path pathFor(std::string_view kind, std::string_view key) const;

        std::filesystem::path _directory;
    };
}
