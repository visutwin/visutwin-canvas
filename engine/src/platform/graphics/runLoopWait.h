// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// A blocking wait that keeps the main thread's run loop serviced.
//
// When the pointer is pressed over a window, macOS holds the SYSTEM pointer until the
// app's main thread services its run loop. A main thread blocked instead (in a semaphore,
// CAMetalLayer::nextDrawable, vkWaitForFences or vkAcquireNextImageKHR: most of a vsynced
// frame) is only given up on after ~200-250 ms, and the press then arrives with all the
// movement made meanwhile folded into a single jump: a click-and-drag that hangs, then
// lurches. An SDL app waits outside the run loop unless it idles in SDL_WaitEvent, so every
// frame loop here has this unless its waits go through this function.
//
#pragma once

#include <functional>

namespace visutwin::canvas
{
    /// Run `wait` (a call that blocks for the display or the GPU) to completion. On macOS,
    /// called from the main thread, it runs on a serial queue of its own while the main thread
    /// waits inside its run loop, woken the moment `wait` returns; anywhere else it is called
    /// directly. `wait` must not need the main thread itself.
    void waitServicingRunLoop(const std::function<void()>& wait);
}
