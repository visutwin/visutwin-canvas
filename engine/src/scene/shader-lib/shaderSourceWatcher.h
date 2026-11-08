// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
// Watches a shader source tree for changes by polling file modification times: every
// .slang file under a directory, recursively. Polling a tree of a few hundred files costs
// well under a millisecond and needs nothing per platform, which a development tool on two
// operating systems prefers to FSEvents and inotify. The engine polls it once a frame when
// VISUTWIN_SHADER_WATCH is set (Engine::pollShaderSources), at most once per interval.
//
#pragma once

#include <chrono>
#include <filesystem>
#include <map>

namespace visutwin::canvas
{
    class ShaderSourceWatcher
    {
    public:
        explicit ShaderSourceWatcher(std::filesystem::path directory,
            std::chrono::milliseconds interval = std::chrono::milliseconds(250));

        /// Whether a .slang file under the directory was added, removed or modified since the
        /// previous look; the first look records the tree and answers false. Looks at most once
        /// per interval (a zero interval looks at every call) and otherwise answers false.
        bool poll();

        const std::filesystem::path& directory() const { return _directory; }

    private:
        using Snapshot = std::map<std::filesystem::path, std::filesystem::file_time_type>;
        Snapshot snapshot() const;

        std::filesystem::path _directory;
        std::chrono::milliseconds _interval;
        std::chrono::steady_clock::time_point _lastLook{};
        Snapshot _files;
        bool _looked = false;
    };
}
