// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.07.2026.
//
#include "shaderChunks.h"

#include <array>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        struct DefaultChunkStore
        {
            std::unordered_map<std::string, std::string> sources;
            std::filesystem::path rootPath;
        };

        std::string readTextFile(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::in | std::ios::binary);
            if (!in) {
                return {};
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        std::filesystem::path projectRootFromThisSource()
        {
            auto path = std::filesystem::path(__FILE__).parent_path();
            for (int i = 0; i < 4; ++i) {
                path = path.parent_path();
            }
            return path;
        }

        std::optional<DefaultChunkStore> loadDefaultChunks()
        {
            const auto sourceRoot = projectRootFromThisSource();
            const auto cwd = std::filesystem::current_path();
            const std::array<std::filesystem::path, 4> chunkRoots = {
                sourceRoot / "engine/shaders/metal/chunks",
                cwd / "engine/shaders/metal/chunks",
                cwd.parent_path() / "engine/shaders/metal/chunks",
                cwd.parent_path().parent_path() / "engine/shaders/metal/chunks"
            };

            for (const auto& root : chunkRoots) {
                if (!std::filesystem::exists(root) || !std::filesystem::is_directory(root)) {
                    continue;
                }

                DefaultChunkStore store;
                store.rootPath = root;
                for (const auto& entry : std::filesystem::directory_iterator(root)) {
                    if (!entry.is_regular_file() || entry.path().extension() != ".metal") {
                        continue;
                    }
                    const auto chunkName = entry.path().stem().string();
                    auto chunkSource = readTextFile(entry.path());
                    if (!chunkSource.empty()) {
                        store.sources[chunkName] = std::move(chunkSource);
                    }
                }

                if (!store.sources.empty()) {
                    spdlog::info("Loaded {} shader chunks from {}", store.sources.size(), root.string());
                    return store;
                }
            }
            return std::nullopt;
        }

        const DefaultChunkStore* defaultChunkStore()
        {
            static std::optional<DefaultChunkStore> store = loadDefaultChunks();
            return store ? &*store : nullptr;
        }

        const std::filesystem::path kEmptyPath;
    }

    ShaderChunks::ShaderChunks()
    {
        const auto* store = defaultChunkStore();
        _defaults = store ? &store->sources : nullptr;
    }

    const std::filesystem::path& ShaderChunks::rootPath() const
    {
        const auto* store = defaultChunkStore();
        return store ? store->rootPath : kEmptyPath;
    }

    const std::string* ShaderChunks::get(const std::string& name) const
    {
        if (const auto overrideIt = _overrides.find(name); overrideIt != _overrides.end()) {
            return &overrideIt->second;
        }
        if (_defaults) {
            if (const auto defaultIt = _defaults->find(name); defaultIt != _defaults->end()) {
                return &defaultIt->second;
            }
        }
        return nullptr;
    }

    bool ShaderChunks::has(const std::string& name) const
    {
        return get(name) != nullptr;
    }

    void ShaderChunks::set(const std::string& name, std::string source)
    {
        if (_defaults && _defaults->find(name) == _defaults->end()) {
            spdlog::info("ShaderChunks: adding non-default chunk '{}'", name);
        }
        _overrides[name] = std::move(source);
        _hash = hashChunkMap(_overrides);
    }

    bool ShaderChunks::remove(const std::string& name)
    {
        const bool removed = _overrides.erase(name) > 0;
        if (removed) {
            _hash = hashChunkMap(_overrides);
        }
        return removed;
    }

    void ShaderChunks::clearOverrides()
    {
        if (!_overrides.empty()) {
            _overrides.clear();
            _hash = 0;
        }
    }

    std::vector<std::string> ShaderChunks::names() const
    {
        std::vector<std::string> result;
        if (_defaults) {
            result.reserve(_defaults->size());
            for (const auto& [name, source] : *_defaults) {
                result.push_back(name);
            }
        }
        return result;
    }

    uint64_t ShaderChunks::hashChunkMap(const std::unordered_map<std::string, std::string>& chunks)
    {
        if (chunks.empty()) {
            return 0;
        }
        // Order-independent: combine per-entry FNV-1a hashes with XOR so the
        // unordered_map iteration order cannot change the fingerprint.
        uint64_t combined = 0;
        for (const auto& [name, source] : chunks) {
            uint64_t hash = 1469598103934665603ull;
            const auto mix = [&hash](const std::string& text) {
                for (const char c : text) {
                    hash ^= static_cast<uint8_t>(c);
                    hash *= 1099511628211ull;
                }
            };
            mix(name);
            hash ^= 0x9e3779b97f4a7c15ull;
            mix(source);
            combined ^= hash;
        }
        // never collide with the "no overrides" sentinel
        return combined ? combined : 1;
    }
}
