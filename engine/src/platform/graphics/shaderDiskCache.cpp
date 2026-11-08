// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026
//
#include "shaderDiskCache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <stdlib.h>
#endif

#include "spdlog/spdlog.h"
#include "core/hash.h"

namespace visutwin::canvas
{
    namespace
    {
        // File layout: magic, format version, key length, payload length (all
        // little-endian uint64), the key, the payload.
        constexpr uint64_t kMagic = 0x3145484341435456ull;   // "VTCACHE1"
        constexpr uint64_t kFormatVersion = 1;
        constexpr size_t kHeaderSize = 4 * sizeof(uint64_t);

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

        bool isHex16(const std::string_view text)
        {
            if (text.size() != 16) {
                return false;
            }
            for (const char c : text) {
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                    return false;
                }
            }
            return true;
        }

        // "<kind>-<16 hex>.bin", kind lower-case letters, digits and dashes.
        bool isEntryName(const std::string_view name)
        {
            constexpr std::string_view suffix = ".bin";
            if (name.size() < 1 + 1 + 16 + suffix.size() || !name.ends_with(suffix)) {
                return false;
            }
            const std::string_view stem = name.substr(0, name.size() - suffix.size());
            const size_t dash = stem.size() - 17;
            if (stem[dash] != '-' || !isHex16(stem.substr(dash + 1))) {
                return false;
            }
            for (const char c : stem.substr(0, dash)) {
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
                    return false;
                }
            }
            return dash > 0;
        }

        // "<entry name>.<16 hex>.tmp", what store() writes before its rename.
        bool isTemporaryName(const std::string_view name)
        {
            constexpr std::string_view suffix = ".tmp";
            if (name.size() < 1 + 16 + suffix.size() || !name.ends_with(suffix)) {
                return false;
            }
            const std::string_view stem = name.substr(0, name.size() - suffix.size());
            if (stem.size() < 17 || stem[stem.size() - 17] != '.' || !isHex16(stem.substr(stem.size() - 16))) {
                return false;
            }
            return isEntryName(stem.substr(0, stem.size() - 17));
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
        file.close();
        // A hit keeps the entry alive for prune(), which goes by modification time (the
        // access time is not reliably kept). Once a day is enough, and costs one stat.
        std::error_code error;
        const auto path = pathFor(kind, key);
        const auto modified = std::filesystem::last_write_time(path, error);
        const auto now = std::filesystem::file_time_type::clock::now();
        if (!error && now - modified > std::chrono::hours(24)) {
            std::filesystem::last_write_time(path, now, error);
        }
        return payload;
    }

    std::optional<uint64_t> ShaderDiskCache::storedSize(const std::string_view kind, const std::string_view key) const
    {
        if (!enabled()) {
            return std::nullopt;
        }
        std::error_code error;
        const auto size = std::filesystem::file_size(pathFor(kind, key), error);
        if (error) {
            return std::nullopt;
        }
        return static_cast<uint64_t>(size);
    }

    ShaderDiskCache::PruneResult ShaderDiskCache::prune(const PruneLimits& limits) const
    {
        PruneResult result;
        if (!enabled()) {
            return result;
        }
        struct Entry
        {
            std::filesystem::path path;
            std::filesystem::file_time_type modified;
            uint64_t size;
        };
        std::vector<Entry> entries;
        const auto now = std::filesystem::file_time_type::clock::now();
        std::error_code error;
        std::filesystem::directory_iterator it(_directory, error);
        if (error) {
            return result;
        }
        const auto remove = [&](const std::filesystem::path& path, const uint64_t size) {
            std::error_code removeError;
            if (std::filesystem::remove(path, removeError)) {
                ++result.filesRemoved;
                result.bytesRemoved += size;
                return true;
            }
            return false;
        };
        for (const auto end = std::filesystem::directory_iterator(); it != end; it.increment(error)) {
            if (error) {
                break;
            }
            std::error_code entryError;
            if (!it->is_regular_file(entryError) || entryError) {
                continue;
            }
            const std::string name = it->path().filename().string();
            const bool temporary = isTemporaryName(name);
            if (!temporary && !isEntryName(name)) {
                continue;
            }
            const auto size = static_cast<uint64_t>(it->file_size(entryError));
            const auto modified = std::filesystem::last_write_time(it->path(), entryError);
            if (entryError) {
                continue;
            }
            const auto age = now - modified;
            if (temporary ? age > limits.temporaryAge : age > limits.maxAge) {
                if (remove(it->path(), size)) {
                    continue;
                }
            }
            if (!temporary) {
                entries.push_back({it->path(), modified, size});
            }
        }

        uint64_t total = 0;
        for (const Entry& entry : entries) {
            total += entry.size;
        }
        if (total > limits.maxTotalBytes) {
            std::sort(entries.begin(), entries.end(),
                [](const Entry& a, const Entry& b) { return a.modified < b.modified; });
            for (const Entry& entry : entries) {
                if (total <= limits.maxTotalBytes) {
                    break;
                }
                if (remove(entry.path, entry.size)) {
                    total -= entry.size;
                }
            }
        }
        result.bytesKept = total;
        return result;
    }

    std::string ShaderDiskCache::executableName()
    {
#if defined(__APPLE__)
        if (const char* name = getprogname()) {
            return name;
        }
        return {};
#elif defined(_WIN32)
        char* path = nullptr;
        if (_get_pgmptr(&path) == 0 && path) {
            return std::filesystem::path(path).stem().string();
        }
        return {};
#else
        std::error_code error;
        const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
        return error ? std::string{} : path.filename().string();
#endif
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
