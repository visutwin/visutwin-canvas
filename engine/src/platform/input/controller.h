// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers  on 18.10.2025.
//
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "gamePads.h"
#include "inputConstants.h"

namespace visutwin::canvas
{
    class Keyboard;
    class Mouse;

    /**
     * A general input handler which handles both mouse and keyboard input assigned
     * to named actions. This allows you to define input handlers separately to
     * defining keyboard/mouse configurations.
     *
     * An ACTION is a name that several inputs can trigger — "jump" bound to both the
     * space bar and a pad's south button — so the code that responds to it never
     * names a device. An AXIS is a name with a positive and a negative binding,
     * reading -1..1, so a keyboard pair and an analogue stick feed the same value.
     *
     * The devices are borrowed, not owned: the application creates them once and the
     * Controller reads whichever it was given. A binding to a device that is absent
     * simply never fires, which is what makes a gamepad binding safe to register on
     * a machine with no gamepad.
     */
    class Controller
    {
    public:
        Controller(Keyboard* keyboard, Mouse* mouse, GamePads* gamepads)
            : _keyboard(keyboard), _mouse(mouse), _gamepads(gamepads) {}

        /// Bind keys to an action. Called again for the same action, the bindings
        /// ADD: an action can be triggered by any of its inputs.
        void registerKeys(const std::string& action, const std::vector<Key>& keys);
        void registerMouse(const std::string& action, MouseButton button);
        void registerPadButton(const std::string& action, PadButton button, size_t padIndex = 0);

        /// Bind an axis to a pair of keys, to a pad stick, or to both. `positive`
        /// drives the axis toward +1 and `negative` toward -1.
        void registerKeyAxis(const std::string& axis, Key positive, Key negative);
        void registerPadAxis(const std::string& axis, PadAxis padAxis, size_t padIndex = 0);

        [[nodiscard]] bool isPressed(const std::string& action) const;
        [[nodiscard]] bool wasPressed(const std::string& action) const;
        [[nodiscard]] bool wasReleased(const std::string& action) const;

        /// -1..1. A key pair gives exactly -1, 0 or 1; a stick gives its deflection,
        /// already dead-zoned by GamePads. With both bound, whichever is further
        /// from centre wins, so a stick nudge does not fight a held key.
        [[nodiscard]] float axis(const std::string& name) const;

        /// Nothing to do per frame — the edge queries read the devices' own
        /// previous-frame state, which Engine updates. Present because upstream has
        /// it and because Engine drives every input object the same way.
        void update() {}

    private:
        struct Binding
        {
            std::vector<Key> keys;
            std::vector<MouseButton> mouseButtons;
            std::vector<std::pair<size_t, PadButton>> padButtons;
        };

        struct AxisBinding
        {
            std::vector<std::pair<Key, Key>> keyPairs;      // positive, negative
            std::vector<std::pair<size_t, PadAxis>> padAxes;
        };

        template<typename KeyTest, typename MouseTest, typename PadTest>
        [[nodiscard]] bool testAction(const std::string& action, KeyTest keyTest,
            MouseTest mouseTest, PadTest padTest) const;

        Keyboard* _keyboard = nullptr;
        Mouse* _mouse = nullptr;
        GamePads* _gamepads = nullptr;

        std::unordered_map<std::string, Binding> _actions;
        std::unordered_map<std::string, AxisBinding> _axes;
    };
}
