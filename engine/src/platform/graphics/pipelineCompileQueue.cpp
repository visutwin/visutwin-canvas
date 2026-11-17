// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.11.2026
//
#include "pipelineCompileQueue.h"

#include <algorithm>

namespace visutwin::canvas
{
    PipelineCompileQueue::PipelineCompileQueue(const unsigned threads)
    {
        const unsigned hardware = std::max(2u, std::thread::hardware_concurrency());
        _threadCount = threads > 0 ? threads : std::clamp(hardware - 1u, 1u, 8u);
    }

    PipelineCompileQueue::~PipelineCompileQueue()
    {
        {
            const std::lock_guard lock(_mutex);
            _stopping = true;
        }
        _wake.notify_all();
        for (std::thread& thread : _threads) {
            thread.join();
        }
    }

    void PipelineCompileQueue::submit(std::function<void()> job)
    {
        {
            const std::lock_guard lock(_mutex);
            if (_threads.empty()) {
                _threads.reserve(_threadCount);
                for (unsigned i = 0; i < _threadCount; ++i) {
                    _threads.emplace_back([this] { workerLoop(); });
                }
            }
            _jobs.push_back(std::move(job));
            _pending.fetch_add(1, std::memory_order_acq_rel);
        }
        _wake.notify_one();
    }

    void PipelineCompileQueue::waitIdle()
    {
        std::unique_lock lock(_mutex);
        _idle.wait(lock, [this] { return _pending.load(std::memory_order_acquire) == 0; });
    }

    void PipelineCompileQueue::workerLoop()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(_mutex);
                // Stopping still drains the queue: a job's owner waits on its result.
                _wake.wait(lock, [this] { return _stopping || !_jobs.empty(); });
                if (_jobs.empty()) {
                    return;
                }
                job = std::move(_jobs.front());
                _jobs.pop_front();
            }
            job();
            {
                const std::lock_guard lock(_mutex);
                if (_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    _idle.notify_all();
                }
            }
        }
    }
}
