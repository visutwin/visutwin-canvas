// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Key and button identifiers, and the event payloads the input devices fire.
//
#pragma once

#include <cstdint>
#include <vector>

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

namespace visutwin::canvas
{
    /**
     * A physical key, by POSITION on the keyboard rather than by the character it
     * produces — W is the key left of S whatever the layout says it types. Upstream
     * keys off the browser's keyCode, which is also positional.
     *
     * DEVIATION: the enumerators ARE the SDL scancodes rather than a private
     * numbering with a translation table. The platform layer is SDL either way (the
     * devices are fed SDL events), a table of a hundred entries is a hundred chances
     * to mistype one, and an unlisted key still works: `static_cast<Key>(
     * SDL_SCANCODE_...)` is a valid Key, which is why this list can stay short
     * without limiting anyone.
     */
    enum class Key : int
    {
        A = SDL_SCANCODE_A, B = SDL_SCANCODE_B, C = SDL_SCANCODE_C, D = SDL_SCANCODE_D,
        E = SDL_SCANCODE_E, F = SDL_SCANCODE_F, G = SDL_SCANCODE_G, H = SDL_SCANCODE_H,
        I = SDL_SCANCODE_I, J = SDL_SCANCODE_J, K = SDL_SCANCODE_K, L = SDL_SCANCODE_L,
        M = SDL_SCANCODE_M, N = SDL_SCANCODE_N, O = SDL_SCANCODE_O, P = SDL_SCANCODE_P,
        Q = SDL_SCANCODE_Q, R = SDL_SCANCODE_R, S = SDL_SCANCODE_S, T = SDL_SCANCODE_T,
        U = SDL_SCANCODE_U, V = SDL_SCANCODE_V, W = SDL_SCANCODE_W, X = SDL_SCANCODE_X,
        Y = SDL_SCANCODE_Y, Z = SDL_SCANCODE_Z,

        Digit0 = SDL_SCANCODE_0, Digit1 = SDL_SCANCODE_1, Digit2 = SDL_SCANCODE_2,
        Digit3 = SDL_SCANCODE_3, Digit4 = SDL_SCANCODE_4, Digit5 = SDL_SCANCODE_5,
        Digit6 = SDL_SCANCODE_6, Digit7 = SDL_SCANCODE_7, Digit8 = SDL_SCANCODE_8,
        Digit9 = SDL_SCANCODE_9,

        F1 = SDL_SCANCODE_F1, F2 = SDL_SCANCODE_F2, F3 = SDL_SCANCODE_F3,
        F4 = SDL_SCANCODE_F4, F5 = SDL_SCANCODE_F5, F6 = SDL_SCANCODE_F6,
        F7 = SDL_SCANCODE_F7, F8 = SDL_SCANCODE_F8, F9 = SDL_SCANCODE_F9,
        F10 = SDL_SCANCODE_F10, F11 = SDL_SCANCODE_F11, F12 = SDL_SCANCODE_F12,

        Left = SDL_SCANCODE_LEFT, Right = SDL_SCANCODE_RIGHT,
        Up = SDL_SCANCODE_UP, Down = SDL_SCANCODE_DOWN,

        Space = SDL_SCANCODE_SPACE, Enter = SDL_SCANCODE_RETURN,
        Escape = SDL_SCANCODE_ESCAPE, Tab = SDL_SCANCODE_TAB,
        Backspace = SDL_SCANCODE_BACKSPACE, Delete = SDL_SCANCODE_DELETE,
        Home = SDL_SCANCODE_HOME, End = SDL_SCANCODE_END,
        PageUp = SDL_SCANCODE_PAGEUP, PageDown = SDL_SCANCODE_PAGEDOWN,

        Minus = SDL_SCANCODE_MINUS, Equals = SDL_SCANCODE_EQUALS,
        LeftBracket = SDL_SCANCODE_LEFTBRACKET, RightBracket = SDL_SCANCODE_RIGHTBRACKET,
        Comma = SDL_SCANCODE_COMMA, Period = SDL_SCANCODE_PERIOD,
        Semicolon = SDL_SCANCODE_SEMICOLON, Apostrophe = SDL_SCANCODE_APOSTROPHE,
        Slash = SDL_SCANCODE_SLASH, Backslash = SDL_SCANCODE_BACKSLASH,
        Grave = SDL_SCANCODE_GRAVE,

        LeftShift = SDL_SCANCODE_LSHIFT, RightShift = SDL_SCANCODE_RSHIFT,
        LeftControl = SDL_SCANCODE_LCTRL, RightControl = SDL_SCANCODE_RCTRL,
        LeftAlt = SDL_SCANCODE_LALT, RightAlt = SDL_SCANCODE_RALT,
        LeftMeta = SDL_SCANCODE_LGUI, RightMeta = SDL_SCANCODE_RGUI,
    };

    enum class MouseButton : int
    {
        None = 0,
        Left = SDL_BUTTON_LEFT,
        Middle = SDL_BUTTON_MIDDLE,
        Right = SDL_BUTTON_RIGHT,
    };

    /// Modifier keys held when an event was produced. Carried on the event rather
    /// than queried afterwards, because by the time a handler runs the key may be up.
    struct KeyModifiers
    {
        bool shift = false;
        bool control = false;
        bool alt = false;
        bool meta = false;
    };

    struct KeyboardEvent
    {
        Key key = Key::A;
        /// The character the key produces under the current layout, 0 when it
        /// produces none (a modifier, an arrow). `key` is the positional identity.
        uint32_t character = 0;
        bool repeat = false;
        KeyModifiers modifiers;
    };

    struct MouseEvent
    {
        /// Position in window pixels, top-left origin — the same space the renderer
        /// reports its size in, so a screen-space pick needs no conversion.
        float x = 0.0f;
        float y = 0.0f;
        /// Movement since the previous move event. Zero on the first event after
        /// attach or focus loss, where there is no previous position to subtract.
        float dx = 0.0f;
        float dy = 0.0f;
        /// Wheel movement, in notches. Positive is away from the user.
        float wheelDelta = 0.0f;
        MouseButton button = MouseButton::None;
        KeyModifiers modifiers;
    };

    struct Touch
    {
        /// Stable for the life of one finger's contact, so a caller can follow it
        /// across move events. SDL reuses ids after release.
        int64_t id = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    struct TouchEvent
    {
        /// Every finger currently in contact, including the ones this event is not
        /// about — a pinch needs both, and only the changed one is in `changed`.
        std::vector<Touch> touches;
        std::vector<Touch> changed;
    };
}
