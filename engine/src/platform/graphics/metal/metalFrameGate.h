// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// How far the CPU may run ahead of the GPU on Metal: kMaxInflightFrames frames, counted
// by one semaphore the device waits on at frame start and the frame's last command
// buffer signals on completion. Everything with a per-frame region (the uniform and
// palette rings, the cluster buffer sets) relies on it: once frameStart has passed the
// gate, the region kMaxInflightFrames frames old is no longer read by the GPU.
//
#pragma once

#include <Metal/Metal.hpp>
#include <dispatch/dispatch.h>

namespace visutwin::canvas
{
    class MetalFrameGate
    {
    public:
        static constexpr int kMaxInflightFrames = 3;

        MetalFrameGate() : _semaphore(dispatch_semaphore_create(kMaxInflightFrames)) {}

        MetalFrameGate(const MetalFrameGate&) = delete;
        MetalFrameGate& operator=(const MetalFrameGate&) = delete;

        /// Frame start: blocks until the frame kMaxInflightFrames back has completed.
        void waitForFrame() { dispatch_semaphore_wait(_semaphore, DISPATCH_TIME_FOREVER); }

        /// Frame end: the frame's slot comes back when `commandBuffer` completes — the LAST
        /// buffer committed for the frame — or at once when there is none, so a frame that
        /// could not get a command buffer does not deadlock the fourth one after it.
        void releaseFrameOnCompletion(MTL::CommandBuffer* commandBuffer)
        {
            dispatch_semaphore_t semaphore = _semaphore;
            if (!commandBuffer) {
                dispatch_semaphore_signal(semaphore);
                return;
            }
            commandBuffer->addCompletedHandler(^(MTL::CommandBuffer*) {
                dispatch_semaphore_signal(semaphore);
            });
        }

        /// Runs `work` with NO frame in flight but the caller's: waits out the other
        /// kMaxInflightFrames - 1, then lets them back. For replacing a buffer whose
        /// regions in-flight frames still read (a ring growing at a frame boundary).
        template <typename Work>
        void withOtherFramesDrained(Work&& work)
        {
            for (int i = 1; i < kMaxInflightFrames; ++i) {
                dispatch_semaphore_wait(_semaphore, DISPATCH_TIME_FOREVER);
            }
            work();
            for (int i = 1; i < kMaxInflightFrames; ++i) {
                dispatch_semaphore_signal(_semaphore);
            }
        }

    private:
        dispatch_semaphore_t _semaphore;
    };
}
