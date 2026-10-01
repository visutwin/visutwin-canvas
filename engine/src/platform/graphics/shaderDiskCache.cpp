// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026.
//
#include "shaderDiskCache.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        // File layout: magic, format version, key length, payload length (all
        // little-endian uint64), the key, the payload.
        constexpr uint64_t kMagic = 0x3145484341435456ull;   // "VTCACHE1"
        constexpr uint64_t kFormatVersion = 1;
        constexpr size_t kHeaderSize = 4 * sizeof(uint64_t);

        uint64_t fnv1a64(const std::string_view text)
        {
            uint64_t hash = 14695981039346656037ull;
            for (const unsigned char c : text) {
                hash ^= c;
                hash *= 1099511628211ull;
            }
            return hash;
        }

        std::string hex(const uint64_t value)
        {
            static constexpr char digits[] = "0123456789abcdef";
            std::string out(16, '0');
            for (int i = 0; i < 16; ++i) {
                out[static_cast<size_t>(15 - i)] = digits[(value >> (i * 4)) & 0xF];
            }
            return out;
        }

        const char* environment(const char* name)
        {
            const char* value = std::getenv(name);
            return (value && *value) ? value : nullptr;
        }
    }

    ShaderDiskCache::ShaderDiskCache(std::filesystem::path directory)
        : _directory(std::move(directory))
    {
    }

    std::filesystem::path ShaderDiskCache::resolveDirectory(const std::string& configured, const bool enabled)
    {
        if (const char* toggle = environment("VISUTWIN_SHADER_CACHE"); toggle && std::string_view(toggle) == "0") {
            return {};
        }
        if (const char* directory = environment("VISUTWIN_SHADER_CACHE_DIR")) {
            return directory;
        }
        if (!enabled) {
            return {};
        }
        if (!configured.empty()) {
            return configured;
        }
#if defined(_WIN32)
        if (const char* local = environment("LOCALAPPDATA")) {
            return std::filesystem::path(local) / "visutwin-canvas" / "cache";
        }
        return {};
#else
        const char* home = environment("HOME");
#if defined(__APPLE__)
        return home ? std::filesystem::path(home) / "Library" / "Caches" / "visutwin-canvas"
                    : std::filesystem::path{};
#else
        if (const char* xdg = environment("XDG_CACHE_HOME")) {
            return std::filesystem::path(xdg) / "visutwin-canvas";
        }
        return home ? std::filesystem::path(home) / ".cache" / "visutwin-canvas" : std::filesystem::path{};
#endif
#endif
    }

    std::filesystem::path ShaderDiskCache::pathFor(const std::string_view kind, const std::string_view key) const
    {
        return _directory / (std::string(kind) + "-" + hex(fnv1a64(key)) + ".bin");
    }

    std::optional<std::vector<uint8_t>> ShaderDiskCache::load(const std::string_view kind,
        const std::string_view key) const
    {
        if (!enabled()) {
            return std::nullopt;
        }
        std::ifstream file(pathFor(kind, key), std::ios::binary | std::ios::ate);
        if (!file) {
            return std::nullopt;
        }
        const auto fileSize = static_cast<uint64_t>(file.tellg());
        if (fileSize < kHeaderSize) {
            return std::nullopt;
        }
        file.seekg(0);
        std::array<uint64_t, 4> header{};
        file.read(reinterpret_cast<char*>(header.data()), kHeaderSize);
        const uint64_t keySize = header[2];
        const uint64_t payloadSize = header[3];
        if (!file || header[0] != kMagic || header[1] != kFormatVersion || keySize != key.size() ||
            payloadSize > fileSize || kHeaderSize + keySize + payloadSize != fileSize) {
            return std::nullopt;
        }
        // The key in full: the name is only a hash of it.
        std::string storedKey(keySize, '\0');
        file.read(storedKey.data(), static_cast<std::streamsize>(keySize));
        if (!file || storedKey != key) {
            return std::nullopt;
        }
        std::vector<uint8_t> payload(payloadSize);
        file.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payloadSize));
        if (!file) {
            return std::nullopt;
        }
        return payload;
    }

    bool ShaderDiskCache::store(const std::string_view kind, const std::string_view key,
        const std::span<const uint8_t> payload) const
    {
        if (!enabled()) {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(_directory, error);
        if (error) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                spdlog::warn("Shader cache: cannot create '{}' ({}); compiled shaders will not be kept",
                    _directory.string(), error.message());
            }
            return false;
        }

        const std::filesystem::path target = pathFor(kind, key);
        // Unique per writer, so two processes storing the same entry do not share a
        // half-written file; whichever renames last wins, with identical contents.
        static std::atomic<uint64_t> counter{0};
        const auto now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::path temporary = target;
        temporary += "." + hex(now ^ (reinterpret_cast<uintptr_t>(this) << 20) ^ counter.fetch_add(1)) + ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) {
                return false;
            }
            const std::array<uint64_t, 4> header{kMagic, kFormatVersion, key.size(), payload.size()};
            file.write(reinterpret_cast<const char*>(header.data()), kHeaderSize);
            file.write(key.data(), static_cast<std::streamsize>(key.size()));
            file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
            if (!file) {
                file.close();
                std::filesystem::remove(temporary, error);
                return false;
            }
        }
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }
}
