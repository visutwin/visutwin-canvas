// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Subscriptions the input sources share: one pointer per finger from the engine's touch
// device (MultiTouchSource, DualGestureSource), and releasing a source's subscriptions on
// detach (every source).
//
#pragma once

#include <utility>
#include <vector>

#include "core/eventHandler.h"
#include "framework/engine.h"
#include "platform/input/touchDevice.h"

namespace visutwin::canvas
{
    /// Unsubscribes and forgets every handle.
    inline void releaseHandles(std::vector<EventHandlePtr>& handles)
    {
        for (const auto& handle : handles) {
            handle->off();
        }
        handles.clear();
    }

    /// Routes each changed finger of the engine's touch device to `down(id, x, y)`,
    /// `move(id, x, y)` and `up(id)` (an end or a cancel), adding the subscriptions to
    /// `handles`. Nothing when the engine has no touch device.
    template <typename Down, typename Move, typename Up>
    void subscribeTouchPointers(Engine* engine, std::vector<EventHandlePtr>& handles, Down down, Move move, Up up)
    {
        TouchDevice* touch = engine ? engine->touch() : nullptr;
        if (!touch) {
            return;
        }
        handles.push_back(touch->on("touchstart", [down = std::move(down)](const TouchEvent& e) {
            for (const Touch& t : e.changed) {
                down(t.id, t.x, t.y);
            }
        }));
        handles.push_back(touch->on("touchmove", [move = std::move(move)](const TouchEvent& e) {
            for (const Touch& t : e.changed) {
                move(t.id, t.x, t.y);
            }
        }));
        const auto ended = [up = std::move(up)](const TouchEvent& e) {
            for (const Touch& t : e.changed) {
                up(t.id);
            }
        };
        handles.push_back(touch->on("touchend", ended));
        handles.push_back(touch->on("touchcancel", ended));
    }
}
