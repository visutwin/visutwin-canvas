// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
#include "shaderSourceWatcher.h"

#include <system_error>
#include <utility>

namespace visutwin::canvas
{
    ShaderSourceWatcher::ShaderSourceWatcher(std::filesystem::path directory, const std::chrono::milliseconds interval)
        : _directory(std::move(directory)), _interval(interval)
    {
    }

    ShaderSourceWatcher::Snapshot ShaderSourceWatcher::snapshot() const
    {
        // Errors are skipped, not thrown: an editor that saves by writing a temporary file and
        // renaming it can remove a file between the listing and the time stamp.
        Snapshot files;
        std::error_code error;
        for (auto it = std::filesystem::recursive_directory_iterator(_directory, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            std::error_code entryError;
            if (!it->is_regular_file(entryError) || it->path().extension() != ".slang") {
                continue;
            }
            const auto time = std::filesystem::last_write_time(it->path(), entryError);
            if (!entryError) {
                files.emplace(it->path(), time);
            }
        }
        return files;
    }

    bool ShaderSourceWatcher::poll()
    {
        const auto now = std::chrono::steady_clock::now();
        if (_looked && now - _lastLook < _interval) {
            return false;
        }
        _lastLook = now;
        auto files = snapshot();
        const bool changed = _looked && files != _files;
        _files = std::move(files);
        _looked = true;
        return changed;
    }
}
