// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026
//
// ShaderDiskCache keeps compiled shader products between runs (the SPIR-V the Vulkan
// backend compiles from GLSL at run time, and its pipeline cache). The one thing it may
// never do is hand back a payload made from something else: an entry is found by a HASH
// of its key and accepted only when the key stored beside the payload is the caller's,
// byte for byte, so a stale file, a damaged one, or two keys with one hash are all
// misses. A wrong hit here would run another shader's code with nothing to show why.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "platform/graphics/shaderDiskCache.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    std::vector<std::filesystem::path> filesIn(const std::filesystem::path& directory)
    {
        std::vector<std::filesystem::path> files;
        if (std::filesystem::exists(directory)) {
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                files.push_back(entry.path());
            }
        }
        return files;
    }
}

int main()
{
    const auto directory = std::filesystem::temp_directory_path() / "visutwin-shader-disk-cache-tests";
    std::filesystem::remove_all(directory);

    const std::string key = "stage 1\ndefine A=1\nvoid main() { }";
    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8, 9};

    std::cout << "store and load\n";
    {
        const ShaderDiskCache cache(directory);
        check(cache.enabled(), "a cache with a directory is enabled");
        check(!cache.load("spirv", key).has_value(), "nothing is found before anything is stored");
        check(!std::filesystem::exists(directory), "and looking does not create the directory");
        check(cache.store("spirv", key, payload), "a payload is stored");
        const auto loaded = cache.load("spirv", key);
        check(loaded && *loaded == payload, "and comes back byte for byte");
        check(filesIn(directory).size() == 1, "as one file, with no temporary left behind");

        check(!cache.load("spirv", key + " ").has_value(), "another key finds nothing");
        check(!cache.load("pipelines", key).has_value(), "nor does the same key under another kind");

        const std::vector<uint8_t> replacement = {42};
        check(cache.store("spirv", key, replacement), "storing again under the key");
        const auto reloaded = cache.load("spirv", key);
        check(reloaded && *reloaded == replacement, "replaces the payload");

        const std::vector<uint8_t> empty;
        check(cache.store("spirv", "empty", empty), "an empty payload is stored");
        const auto loadedEmpty = cache.load("spirv", "empty");
        check(loadedEmpty && loadedEmpty->empty(), "and loads as empty, not as a miss");
    }

    std::cout << "\na second cache on the same directory (the next run)\n";
    {
        const ShaderDiskCache nextRun(directory);
        const auto loaded = nextRun.load("spirv", key);
        check(loaded && loaded->size() == 1 && (*loaded)[0] == 42, "finds what the first one stored");
    }

    std::cout << "\nfiles that must not be accepted\n";
    {
        const ShaderDiskCache cache(directory);
        std::filesystem::remove_all(directory);
        cache.store("spirv", key, payload);
        const auto files = filesIn(directory);
        check(files.size() == 1, "one entry to damage");
        const auto path = files.empty() ? std::filesystem::path() : files.front();

        std::vector<char> original;
        {
            std::ifstream in(path, std::ios::binary);
            original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const auto rewrite = [&path](const std::vector<char>& bytes) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        };
        const size_t headerSize = 4 * sizeof(uint64_t);

        // The key stored in the file is another key of the same length: what a hash
        // collision between two keys would look like on disk.
        auto otherKey = original;
        otherKey[headerSize] ^= 0x20;
        rewrite(otherKey);
        check(!cache.load("spirv", key).has_value(), "an entry whose stored key differs is a miss");

        auto truncated = original;
        truncated.resize(truncated.size() - 3);
        rewrite(truncated);
        check(!cache.load("spirv", key).has_value(), "a truncated file is a miss");

        auto longer = original;
        longer.push_back('x');
        rewrite(longer);
        check(!cache.load("spirv", key).has_value(), "a file with bytes after the payload is a miss");

        auto badMagic = original;
        badMagic[0] ^= 0x7F;
        rewrite(badMagic);
        check(!cache.load("spirv", key).has_value(), "a file that is not a cache entry is a miss");

        auto badVersion = original;
        badVersion[sizeof(uint64_t)] ^= 0x7F;
        rewrite(badVersion);
        check(!cache.load("spirv", key).has_value(), "an entry of another format version is a miss");

        rewrite({});
        check(!cache.load("spirv", key).has_value(), "an empty file is a miss");

        rewrite(original);
        const auto restored = cache.load("spirv", key);
        check(restored && *restored == payload, "the undamaged file is still accepted");
    }

    std::cout << "\nstored size\n";
    {
        std::filesystem::remove_all(directory);
        const ShaderDiskCache cache(directory);
        check(!cache.storedSize("pipelines", key).has_value(), "nothing stored has no size");
        cache.store("pipelines", key, payload);
        const auto size = cache.storedSize("pipelines", key);
        check(size && *size == 4 * sizeof(uint64_t) + key.size() + payload.size(),
            "a stored entry's size is header, key and payload");
    }

    std::cout << "\npruning\n";
    {
        using namespace std::chrono_literals;
        std::filesystem::remove_all(directory);
        const ShaderDiskCache cache(directory);
        const auto now = std::filesystem::file_time_type::clock::now();
        const auto pathOf = [&](const std::string& kind, const std::string& entryKey) {
            // The file the store just wrote: the one that was not there before it. Not the newest
            // by time: Linux stamps files from a coarse clock, so two stores a few milliseconds
            // apart share a time and the newest is whichever the directory lists last.
            const auto before = filesIn(directory);
            cache.store(kind, entryKey, payload);
            std::filesystem::path written;
            for (const auto& file : filesIn(directory)) {
                if (file.filename().string().starts_with(kind + "-") &&
                    std::find(before.begin(), before.end(), file) == before.end()) {
                    check(written.empty(), "one store writes one file");
                    written = file;
                }
            }
            return written;
        };
        const auto fresh = pathOf("spirv", "fresh");
        const auto old = pathOf("spirv", "old");
        std::filesystem::last_write_time(old, now - 24h * 30);
        const auto readOld = pathOf("slang-frag", "read but old");
        std::filesystem::last_write_time(readOld, now - 24h * 30);
        check(cache.load("slang-frag", "read but old").has_value(), "an old entry is read");
        check(now - std::filesystem::last_write_time(readOld) < 1h, "and the hit moves its time to now");

        // A writer that died mid-store, and one storing right now.
        auto deadTemporary = fresh;
        deadTemporary += ".0123456789abcdef.tmp";
        auto liveTemporary = fresh;
        liveTemporary += ".fedcba9876543210.tmp";
        std::ofstream(deadTemporary) << "half";
        std::ofstream(liveTemporary) << "half";
        std::filesystem::last_write_time(deadTemporary, now - 3h);
        // Files the cache did not name, however old, are not its to remove.
        const auto foreign = directory / "notes.bin";
        const auto foreignHex = directory / "spirv-0123.bin";
        std::ofstream(foreign) << "keep";
        std::ofstream(foreignHex) << "keep";
        std::filesystem::last_write_time(foreign, now - 24h * 365);
        std::filesystem::last_write_time(foreignHex, now - 24h * 365);

        const auto result = cache.prune();
        check(result.filesRemoved == 2, "prune removes two files (" + std::to_string(result.filesRemoved) + ")");
        check(!std::filesystem::exists(old), "the entry not touched for a month");
        check(!std::filesystem::exists(deadTemporary), "and the temporary file of a writer that died");
        check(std::filesystem::exists(fresh) && std::filesystem::exists(readOld),
            "while a fresh entry and one just read stay");
        check(std::filesystem::exists(liveTemporary), "as does a temporary file being written now");
        check(std::filesystem::exists(foreign) && std::filesystem::exists(foreignHex),
            "and files the cache did not name");
        check(cache.load("spirv", "fresh").has_value(), "a kept entry still loads");

        // Over the size budget, the oldest entries go first.
        ShaderDiskCache::PruneLimits tight;
        std::filesystem::last_write_time(readOld, now - 2h);
        std::filesystem::last_write_time(fresh, now - 1h);
        const auto newest = pathOf("spirv", "newest");
        const uint64_t keptSize = *cache.storedSize("spirv", "fresh") + *cache.storedSize("spirv", "newest");
        tight.maxTotalBytes = keptSize;
        const auto squeezed = cache.prune(tight);
        check(squeezed.filesRemoved == 1 && !std::filesystem::exists(readOld),
            "a budget of two entries removes the oldest of three");
        check(std::filesystem::exists(fresh) && std::filesystem::exists(newest) &&
            squeezed.bytesKept == keptSize, "and keeps the two newest");

        check(ShaderDiskCache().prune().filesRemoved == 0, "a disabled cache prunes nothing");
    }

    std::cout << "\nthe executable's name\n";
    {
        const std::string name = ShaderDiskCache::executableName();
        check(name.find("shaderDiskCache") != std::string::npos ||
            name.find("shader-disk-cache") != std::string::npos ||
            name.find("shader_disk_cache") != std::string::npos,
            "names this test (" + name + ")");
        check(name.find('/') == std::string::npos, "without its directory");
    }

    std::cout << "\ndisabled\n";
    {
        const ShaderDiskCache off;
        check(!off.enabled(), "a cache with no directory is disabled");
        check(!off.store("spirv", key, payload) && !off.load("spirv", key).has_value(),
            "and neither stores nor finds");
    }

    std::cout << "\nwhere a device caches\n";
    {
        setenv("VISUTWIN_SHADER_CACHE_DIR", "", 1);
        setenv("VISUTWIN_SHADER_CACHE", "", 1);
        check(ShaderDiskCache::resolveDirectory("/some/where", true) == std::filesystem::path("/some/where"),
            "the application's directory is used as given");
        check(ShaderDiskCache::resolveDirectory("/some/where", false).empty(), "unless caching is off");
        const auto fallback = ShaderDiskCache::resolveDirectory("", true);
        check(!fallback.empty() && fallback.string().find("visutwin-canvas") != std::string::npos,
            "with none given, a per-user directory named for the engine (" + fallback.string() + ")");
        setenv("VISUTWIN_SHADER_CACHE_DIR", "/from/the/environment", 1);
        check(ShaderDiskCache::resolveDirectory("/some/where", true) == std::filesystem::path("/from/the/environment"),
            "VISUTWIN_SHADER_CACHE_DIR overrides the application's choice");
        check(ShaderDiskCache::resolveDirectory("", false) == std::filesystem::path("/from/the/environment"),
            "and turns caching on for an application that left it off");
        setenv("VISUTWIN_SHADER_CACHE", "0", 1);
        check(ShaderDiskCache::resolveDirectory("/some/where", true).empty(), "VISUTWIN_SHADER_CACHE=0 turns it off");
        unsetenv("VISUTWIN_SHADER_CACHE");
        unsetenv("VISUTWIN_SHADER_CACHE_DIR");
    }

    std::filesystem::remove_all(directory);
    return finish("shader disk cache");
}
