// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The input devices are state machines driven by a window-system event stream, and
// nothing about them is visible in a rendered frame: an example screenshot is
// identical whether the keyboard tracks keys correctly or not at all. So they are
// driven here instead, with synthetic SDL events, which needs no window, no device
// and no one at the keyboard.
//
// What is worth pinning is the part a per-frame poll cannot reconstruct: the EDGES.
// isPressed is what SDL would have told you anyway; wasPressed and wasReleased are
// the reason these classes exist, and they depend on update() being called exactly
// once per frame and on the previous-frame snapshot it takes.

#include <iostream>
#include <string>

#include "platform/input/controller.h"
#include "platform/input/gamePads.h"
#include "platform/input/keyboard.h"
#include "platform/input/mouse.h"
#include "platform/input/touchDevice.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        if (!condition) {
            std::cerr << "FAILED: " << what << "\n";
            ++failures;
        }
    }

    void checkNear(const float actual, const float expected, const std::string& what)
    {
        if (std::abs(actual - expected) > 1e-4f) {
            std::cerr << "FAILED: " << what << " (got " << actual
                      << ", expected " << expected << ")\n";
            ++failures;
        }
    }

    SDL_Event keyEvent(const bool down, const SDL_Scancode scancode,
        const SDL_Keymod mod = SDL_KMOD_NONE)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.scancode = scancode;
        event.key.key = SDL_GetKeyFromScancode(scancode, mod, false);
        event.key.mod = mod;
        return event;
    }

    SDL_Event mouseButtonEvent(const bool down, const uint8_t button, const float x, const float y)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.button = button;
        event.button.x = x;
        event.button.y = y;
        return event;
    }

    SDL_Event motionEvent(const float x, const float y, const float dx, const float dy)
    {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.x = x;
        event.motion.y = y;
        event.motion.xrel = dx;
        event.motion.yrel = dy;
        return event;
    }

    SDL_Event fingerEvent(const uint32_t type, const SDL_FingerID id, const float x, const float y)
    {
        SDL_Event event{};
        event.type = type;
        event.tfinger.fingerID = id;
        event.tfinger.x = x;
        event.tfinger.y = y;
        return event;
    }

    SDL_Event focusLostEvent()
    {
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        return event;
    }

    void keyboardStateAndEdges()
    {
        Keyboard keyboard;
        check(!keyboard.isPressed(Key::W), "a fresh keyboard holds no key");

        keyboard.handleEvent(keyEvent(true, SDL_SCANCODE_W));
        check(keyboard.isPressed(Key::W), "a key down reads as pressed");
        check(keyboard.wasPressed(Key::W), "a key down is a press edge in the frame it arrives");
        check(!keyboard.wasReleased(Key::W), "a key down is not a release edge");
        check(!keyboard.isPressed(Key::S), "an untouched key stays up");

        keyboard.update();
        check(keyboard.isPressed(Key::W), "a held key stays pressed across a frame");
        check(!keyboard.wasPressed(Key::W), "the press edge lasts one frame only");

        keyboard.handleEvent(keyEvent(false, SDL_SCANCODE_W));
        check(!keyboard.isPressed(Key::W), "a key up reads as released");
        check(keyboard.wasReleased(Key::W), "a key up is a release edge");

        keyboard.update();
        check(!keyboard.wasReleased(Key::W), "the release edge lasts one frame only");

        // The case a poll of the current state cannot see at all: pressed and
        // released between two frames. Both edges have to be true in that frame.
        keyboard.handleEvent(keyEvent(true, SDL_SCANCODE_SPACE));
        keyboard.handleEvent(keyEvent(false, SDL_SCANCODE_SPACE));
        check(!keyboard.isPressed(Key::Space), "a tapped key is not held afterwards");
        check(keyboard.wasReleased(Key::Space), "a tap inside one frame still reports its release");
    }

    void keyboardModifiersAndFocus()
    {
        Keyboard keyboard;
        keyboard.handleEvent(keyEvent(true, SDL_SCANCODE_RSHIFT));
        check(keyboard.shift(), "either shift key answers shift()");
        keyboard.handleEvent(keyEvent(true, SDL_SCANCODE_LCTRL));
        check(keyboard.control(), "either control key answers control()");
        check(!keyboard.alt(), "alt is not reported when it is not held");

        // A key held while the window loses focus never sends its key-up, so without
        // this it would read as held for the rest of the process.
        keyboard.handleEvent(keyEvent(true, SDL_SCANCODE_W));
        keyboard.handleEvent(focusLostEvent());
        check(!keyboard.isPressed(Key::W), "losing focus releases held keys");
        check(!keyboard.shift(), "losing focus releases held modifiers");
    }

    void keyboardEvents()
    {
        Keyboard keyboard;
        int downs = 0;
        int ups = 0;
        Key lastKey = Key::A;
        keyboard.on("keydown", [&](const KeyboardEvent& event) {
            ++downs;
            lastKey = event.key;
        });
        keyboard.on("keyup", [&](const KeyboardEvent&) { ++ups; });

        keyboard.handleEvent(keyEvent(true, SDL_SCANCODE_K));
        keyboard.handleEvent(keyEvent(false, SDL_SCANCODE_K));
        check(downs == 1 && ups == 1, "keydown and keyup each fire once per event");
        check(lastKey == Key::K, "the event carries the key that changed");
    }

    void mouseButtonsPositionAndWheel()
    {
        Mouse mouse;
        mouse.handleEvent(mouseButtonEvent(true, SDL_BUTTON_LEFT, 10.0f, 20.0f));
        check(mouse.isPressed(MouseButton::Left), "a button down reads as pressed");
        check(mouse.wasPressed(MouseButton::Left), "a button down is a press edge");
        check(!mouse.isPressed(MouseButton::Right), "an untouched button stays up");
        checkNear(mouse.x(), 10.0f, "the button event updates the position");
        checkNear(mouse.y(), 20.0f, "the button event updates the position");

        mouse.update();
        check(!mouse.wasPressed(MouseButton::Left), "the press edge lasts one frame only");
        check(mouse.isPressed(MouseButton::Left), "a held button stays pressed");

        // Two moves in one frame: the per-frame delta is their SUM, which is the
        // reason it is accumulated rather than overwritten.
        mouse.handleEvent(motionEvent(12.0f, 24.0f, 2.0f, 4.0f));
        mouse.handleEvent(motionEvent(15.0f, 25.0f, 3.0f, 1.0f));
        checkNear(mouse.deltaX(), 5.0f, "movement accumulates across a frame");
        checkNear(mouse.deltaY(), 5.0f, "movement accumulates across a frame");
        checkNear(mouse.x(), 15.0f, "the position is the latest, not the sum");

        mouse.handleEvent(mouseButtonEvent(false, SDL_BUTTON_LEFT, 15.0f, 25.0f));
        check(mouse.wasReleased(MouseButton::Left), "a button up is a release edge");

        SDL_Event wheel{};
        wheel.type = SDL_EVENT_MOUSE_WHEEL;
        wheel.wheel.y = 1.5f;
        mouse.handleEvent(wheel);
        mouse.handleEvent(wheel);
        checkNear(mouse.wheelDelta(), 3.0f, "wheel notches accumulate across a frame");

        mouse.update();
        checkNear(mouse.deltaX(), 0.0f, "movement resets at the frame boundary");
        checkNear(mouse.wheelDelta(), 0.0f, "the wheel resets at the frame boundary");

        mouse.handleEvent(mouseButtonEvent(true, SDL_BUTTON_MIDDLE, 0.0f, 0.0f));
        mouse.handleEvent(focusLostEvent());
        check(!mouse.isPressed(MouseButton::Middle), "losing focus releases held buttons");
    }

    void touchTracksFingers()
    {
        TouchDevice touch;
        touch.setWindowSize(800, 600);

        int starts = 0;
        size_t touchesAtEnd = 0;
        touch.on("touchstart", [&](const TouchEvent&) { ++starts; });
        touch.on("touchend", [&](const TouchEvent& event) {
            touchesAtEnd = event.touches.size();
        });

        touch.handleEvent(fingerEvent(SDL_EVENT_FINGER_DOWN, 1, 0.5f, 0.25f));
        check(touch.touches().size() == 1, "a finger down is tracked");
        // SDL reports normalized window coordinates; a caller working in pixels
        // would otherwise place every touch in the top-left corner.
        checkNear(touch.touches()[0].x, 400.0f, "touch x is converted to pixels");
        checkNear(touch.touches()[0].y, 150.0f, "touch y is converted to pixels");

        touch.handleEvent(fingerEvent(SDL_EVENT_FINGER_DOWN, 2, 0.25f, 0.5f));
        check(touch.touches().size() == 2, "a second finger is tracked alongside the first");
        check(starts == 2, "each finger down fires touchstart");

        touch.handleEvent(fingerEvent(SDL_EVENT_FINGER_MOTION, 1, 0.75f, 0.25f));
        checkNear(touch.touches()[0].x, 600.0f, "a move updates that finger only");
        checkNear(touch.touches()[1].x, 200.0f, "a move updates that finger only");

        touch.handleEvent(fingerEvent(SDL_EVENT_FINGER_UP, 1, 0.75f, 0.25f));
        check(touch.touches().size() == 1, "a finger up is untracked");
        check(touch.touches()[0].id == 2, "the remaining finger keeps its identity");
        // The list on the event must not contain the finger that just left, or a
        // pinch handler counts two fingers after one has gone.
        check(touchesAtEnd == 1, "touchend reports the fingers that REMAIN");

        touch.handleEvent(focusLostEvent());
        check(touch.touches().empty(), "losing focus drops every finger");
    }

    void controllerActionsAndAxes()
    {
        Keyboard keyboard;
        Mouse mouse;
        GamePads gamepads;
        Controller controller(&keyboard, &mouse, &gamepads);

        controller.registerKeys("jump", {Key::Space, Key::W});
        controller.registerMouse("fire", MouseButton::Left);
        controller.registerKeyAxis("move", Key::D, Key::A);

        check(!controller.isPressed("jump"), "an unbound input leaves its action inactive");
        keyboard.handleEvent(keyEvent(true, SDL_SCANCODE_W));
        check(controller.isPressed("jump"), "either bound key triggers the action");
        check(controller.wasPressed("jump"), "the action reports the press edge");
        keyboard.update();
        check(!controller.wasPressed("jump"), "the action's press edge lasts one frame");

        mouse.handleEvent(mouseButtonEvent(true, SDL_BUTTON_LEFT, 0.0f, 0.0f));
        check(controller.isPressed("fire"), "a mouse binding triggers its action");
        check(!controller.isPressed("nothing"), "an unregistered action is never pressed");

        checkNear(controller.axis("move"), 0.0f, "an axis with nothing held reads zero");
        keyboard.handleEvent(keyEvent(true, SDL_SCANCODE_D));
        checkNear(controller.axis("move"), 1.0f, "the positive key drives the axis to +1");
        keyboard.handleEvent(keyEvent(true, SDL_SCANCODE_A));
        // Both directions held means neither, rather than whichever was registered
        // first — the alternative makes the result depend on declaration order.
        checkNear(controller.axis("move"), 0.0f, "opposite keys held together cancel");
        keyboard.handleEvent(keyEvent(false, SDL_SCANCODE_D));
        checkNear(controller.axis("move"), -1.0f, "the negative key drives the axis to -1");
        checkNear(controller.axis("unbound"), 0.0f, "an unregistered axis reads zero");
    }

    void gamePadsWithNoPad()
    {
        // Every query has to be safe with nothing connected: an application binds a
        // pad at startup, long before one is plugged in, and the bindings must not
        // be conditional on that.
        GamePads gamepads;
        gamepads.update();
        check(gamepads.count() == 0, "no pads are reported when none are connected");
        check(!gamepads.isPressed(0, PadButton::South), "a query on a missing pad is false");
        check(!gamepads.wasPressed(3, PadButton::Start), "an out-of-range pad index is false");
        checkNear(gamepads.axis(0, PadAxis::LeftX), 0.0f, "a missing pad's axes read zero");
        check(gamepads.name(0).empty(), "a missing pad has no name");
    }
}

int main()
{
    keyboardStateAndEdges();
    keyboardModifiersAndFocus();
    keyboardEvents();
    mouseButtonsPositionAndWheel();
    touchTracksFingers();
    controllerActionsAndAxes();
    gamePadsWithNoPad();

    if (failures > 0) {
        std::cerr << "input device tests FAILED (" << failures << ")\n";
        return 1;
    }
    std::cout << "input device tests passed\n";
    return 0;
}
