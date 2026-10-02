// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The subscription half of the rule for a raw Entity* an object keeps (AGENTS.md):
// subscribe to the entity's "destroy" event when the pointer is set, clear the pointer in
// the handler, and unsubscribe when the pointer changes and in the owner's destructor —
// a handler that outlives its owner is a use-after-free. The owner keeps its own pointer
// and clears it in `onDestroyed`; this keeps the handle and does the unsubscribing.
//
#pragma once

#include <functional>
#include <utility>

#include "core/eventHandler.h"

namespace visutwin::canvas
{
    class DestroyWatch
    {
    public:
        DestroyWatch() = default;
        ~DestroyWatch() { reset(); }

        DestroyWatch(const DestroyWatch&) = delete;
        DestroyWatch& operator=(const DestroyWatch&) = delete;

        /// Stops watching whatever was watched, then runs `onDestroyed` when `emitter` (an
        /// Entity, usually) fires "destroy". A null emitter just stops watching.
        /// `onDestroyed` must not call watch() or reset() on this watch: the handle it runs
        /// from would be freed under it. The next watch(), reset() or the destructor
        /// unsubscribes it.
        void watch(EventHandler* emitter, std::function<void()> onDestroyed)
        {
            reset();
            if (emitter) {
                _handle = emitter->on("destroy", [callback = std::move(onDestroyed)]() { callback(); });
            }
        }

        /// Stops watching. Safe after the emitter is gone: its destructor detaches the handle.
        void reset()
        {
            if (_handle) {
                _handle->off();
                _handle.reset();
            }
        }

        [[nodiscard]] bool watching() const { return _handle != nullptr; }

    private:
        EventHandlePtr _handle;
    };
}
