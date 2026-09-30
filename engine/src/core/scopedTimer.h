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
#include <ctime>

namespace visutwin::canvas
{
    /**
     * CPU time the CALLING THREAD has run, in milliseconds, or a negative value where the
     * platform cannot say. Unlike a wall clock it does not advance while the thread sleeps
     * — in a semaphore, a fence wait, the display — which is what lets a caller tell work
     * from waiting inside a call that does some of both (see DisplayWaitScope).
     */
    inline double threadCpuMilliseconds()
    {
#if defined(CLOCK_THREAD_CPUTIME_ID)
        timespec now{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0) {
            return static_cast<double>(now.tv_sec) * 1000.0 + static_cast<double>(now.tv_nsec) * 1e-6;
        }
#endif
        return -1.0;
    }

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
