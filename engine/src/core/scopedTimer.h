// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Adds the milliseconds a scope took to a running total: what the frame statistics
// counters (Renderer::_cullTime, _forwardTime, ...) accumulate between two
// Engine::fillFrameStats calls. Fractional milliseconds on purpose — a per-phase cost
// is usually well under one, and whole-millisecond truncation read as zero.
//
#pragma once

#include <chrono>

namespace visutwin::canvas
{
    class ScopedMilliseconds
    {
    public:
        explicit ScopedMilliseconds(double& total)
            : _total(total), _start(std::chrono::steady_clock::now()) {}

        ~ScopedMilliseconds()
        {
            _total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _start).count();
        }

        ScopedMilliseconds(const ScopedMilliseconds&) = delete;
        ScopedMilliseconds& operator=(const ScopedMilliseconds&) = delete;

    private:
        double& _total;
        std::chrono::steady_clock::time_point _start;
    };
}
