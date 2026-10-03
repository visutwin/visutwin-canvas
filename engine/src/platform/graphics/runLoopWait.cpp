// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
#include "runLoopWait.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <pthread.h>

#include <atomic>
#endif

namespace visutwin::canvas
{
#if defined(__APPLE__)
    namespace
    {
        struct WaitJob
        {
            const std::function<void()>* wait = nullptr;
            CFRunLoopRef mainLoop = nullptr;
            CFRunLoopSourceRef wake = nullptr;
            std::atomic<bool> done{false};
        };

        void wakeOnly(void*) {}

        void runWaitJob(void* context)
        {
            auto* job = static_cast<WaitJob*>(context);
            (*job->wait)();
            // Read before `done`: the waiting thread may return, ending the job, the moment it
            // sees `done`.
            CFRunLoopRef mainLoop = job->mainLoop;
            CFRunLoopSourceRef wake = job->wake;
            job->done.store(true, std::memory_order_release);
            CFRunLoopSourceSignal(wake);
            CFRunLoopWakeUp(mainLoop);
        }

        // One queue and one wake-up source for the process, made on first use on the main
        // thread and kept for its lifetime.
        struct WaitContext
        {
            dispatch_queue_t queue = nullptr;
            CFRunLoopSourceRef wake = nullptr;
        };

        const WaitContext& waitContext()
        {
            static const WaitContext context = [] {
                WaitContext made;
                made.queue = dispatch_queue_create("com.visutwin.runloop-wait", DISPATCH_QUEUE_SERIAL);
                CFRunLoopSourceContext sourceContext{};
                sourceContext.perform = wakeOnly;
                made.wake = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &sourceContext);
                if (made.wake) {
                    CFRunLoopAddSource(CFRunLoopGetMain(), made.wake, kCFRunLoopCommonModes);
                }
                return made;
            }();
            return context;
        }
    }

    void waitServicingRunLoop(const std::function<void()>& wait)
    {
        if (!pthread_main_np()) {
            wait();
            return;
        }
        const WaitContext& context = waitContext();
        if (!context.queue || !context.wake) {
            wait();
            return;
        }
        WaitJob job;
        job.wait = &wait;
        job.mainLoop = CFRunLoopGetMain();
        job.wake = context.wake;
        dispatch_async_f(context.queue, &job, runWaitJob);
        while (!job.done.load(std::memory_order_acquire)) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
        }
    }
#else
    void waitServicingRunLoop(const std::function<void()>& wait)
    {
        wait();
    }
#endif
}
