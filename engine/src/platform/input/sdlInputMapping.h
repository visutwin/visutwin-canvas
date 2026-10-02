// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// SDL's modifier state and mouse buttons in the engine's terms, for every place that
// turns an SDL event into one: the keyboard and mouse devices and the UI's element input.
//
#pragma once

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

#include "platform/input/inputConstants.h"

namespace visutwin::canvas
{
    inline KeyModifiers keyModifiersFromSdl(const SDL_Keymod mod)
    {
        return KeyModifiers{
            .shift = (mod & SDL_KMOD_SHIFT) != 0,
            .control = (mod & SDL_KMOD_CTRL) != 0,
            .alt = (mod & SDL_KMOD_ALT) != 0,
            .meta = (mod & SDL_KMOD_GUI) != 0,
        };
    }

    /// The modifiers held now, for events (mouse) that do not carry their own.
    inline KeyModifiers currentKeyModifiers()
    {
        return keyModifiersFromSdl(SDL_GetModState());
    }

    inline MouseButton mouseButtonFromSdl(const Uint8 sdlButton)
    {
        switch (sdlButton) {
        case SDL_BUTTON_LEFT:   return MouseButton::Left;
        case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
        case SDL_BUTTON_RIGHT:  return MouseButton::Right;
        default:                return MouseButton::None;
        }
    }
}
