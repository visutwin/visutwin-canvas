// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#pragma once

#include <unordered_map>

#include <SDL3/SDL_events.h>

#include "core/eventHandler.h"
#include "inputConstants.h"

namespace visutwin::canvas
{
    /**
     * Manages keyboard input by tracking key states and dispatching events.
     *
     * Two ways to read it, and they answer different questions. `isPressed` is the
     * state right now, for something that happens continuously while a key is held
     * (walking forward). `wasPressed` / `wasReleased` are EDGES since the last
     * `update`, for something that happens once per press (toggling a mode).
     *
     * DEVIATION: upstream derives its edges by comparing this frame's key map
     * against last frame's, which cannot see a key pressed AND released inside one
     * frame — the comparison finds it down in neither snapshot. Transitions are
     * recorded as they arrive instead, so such a tap reports both edges in the frame
     * it happened. Auto-repeat is not a new press: the press edge is recorded only
     * when the key was not already down.
     *
     * Events (`keydown`, `keyup`) carry a KeyboardEvent and fire as the input
     * arrives, for a caller that wants the character or the modifiers as well.
     *
     * DEVIATION: upstream attaches to a DOM element and installs listeners. There
     * is no equivalent to subscribe to here — SDL delivers input by pumping one
     * queue — so the application forwards events in (Engine::handleInputEvent does
     * it for every device at once). `attached` reflects whether that is happening.
     */
    class Keyboard : public EventHandler
    {
    public:
        /// Feed one SDL event. Ignores everything that is not a key event, so the
        /// caller can forward the whole stream without filtering.
        void handleEvent(const SDL_Event& event);

        /// Stop tracking and release every held key. Called when the window loses
        /// focus as well as at teardown: a key held while focus moves away produces
        /// no key-up, and would otherwise read as held forever.
        void detach();

        /// Ends the frame: clears the recorded transitions, so wasPressed and
        /// wasReleased describe the frame that just ran. Engine calls it.
        void update();

        [[nodiscard]] bool isPressed(Key key) const;
        [[nodiscard]] bool wasPressed(Key key) const;
        [[nodiscard]] bool wasReleased(Key key) const;

        /// True while any of shift/control/alt/meta is held, either side.
        [[nodiscard]] bool shift() const;
        [[nodiscard]] bool control() const;
        [[nodiscard]] bool alt() const;
        [[nodiscard]] bool meta() const;

        [[nodiscard]] bool attached() const { return _attached; }

    private:
        std::unordered_map<int, bool> _keys;
        /// Keys whose transition arrived since the last update(). A key can be in
        /// both when it was tapped inside one frame.
        std::unordered_map<int, bool> _pressedThisFrame;
        std::unordered_map<int, bool> _releasedThisFrame;
        bool _attached = false;
    };
}
