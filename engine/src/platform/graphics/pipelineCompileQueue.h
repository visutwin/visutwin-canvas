// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.11.2026
//
// A few worker threads that build render pipelines in parallel for a precompile
// (Engine::precompile): each backend's pipeline cache hands a job here for every pipeline a
// precompile pass asks for and it does not hold yet, and a draw that needs one still building
// waits for that job instead of building it again. The threads start with the first job and
// are joined, after finishing what was queued, when the queue is destroyed, which the owning
// cache does before it frees its pipelines.
//
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace visutwin::canvas
{
    class PipelineCompileQueue
    {
    public:
        /// `threads` 0 takes the hardware's thread count less one (the render thread), 1 to 8.
        explicit PipelineCompileQueue(unsigned threads = 0);
        ~PipelineCompileQueue();

        PipelineCompileQueue(const PipelineCompileQueue&) = delete;
        PipelineCompileQueue& operator=(const PipelineCompileQueue&) = delete;

        /// Runs `job` on a worker thread.
        void submit(std::function<void()> job);

        /// Jobs queued or running.
        [[nodiscard]] int pending() const { return _pending.load(std::memory_order_acquire); }

        /// Blocks until no job is queued or running.
        void waitIdle();

    private:
        void workerLoop();

        unsigned _threadCount;
        std::vector<std::thread> _threads;
        std::deque<std::function<void()>> _jobs;
        std::mutex _mutex;
        std::condition_variable _wake;
        std::condition_variable _idle;
        std::atomic<int> _pending{0};
        bool _stopping = false;
    };
}
