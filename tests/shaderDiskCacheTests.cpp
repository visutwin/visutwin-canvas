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

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "platform/graphics/shaderDiskCache.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

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
    std::cout << (failures == 0 ? "\nAll shader disk cache tests passed\n" : "\nShader disk cache tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
