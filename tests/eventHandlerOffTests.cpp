// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// EventHandler::off(name, callback) removes the handlers registered with the SAME
// function, and only those. on() wraps every callable in a std::function, which cannot
// be compared, so each handler remembers the function it was made from (a function
// pointer, or the one function a captureless lambda converts to) and off() compares
// that. An off() that compares the wrapped std::functions finds nothing, and every
// "removed" handler goes on firing.
//
// A callable with no identity (a capturing lambda, a std::function of another
// signature) is a compile-time error in off(); the concept that decides it is pinned
// here with static_asserts, since a test cannot hold a program that does not compile.

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "core/eventHandler.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    class Emitter final : public EventHandler
    {
    };

    std::vector<std::string> calls;

    void onValueA(const float) { calls.emplace_back("a"); }
    void onValueB(const float) { calls.emplace_back("b"); }
    void onRaw(const EventArgs&) { calls.emplace_back("raw"); }
    void onNothing() { calls.emplace_back("nothing"); }

    int capturedCount = 0;
    const auto captureless = [](const float) { calls.emplace_back("lambda"); };
    const auto capturing = [count = &capturedCount](const float) { ++*count; };

    static_assert(ComparableEventCallback<void (*)(float)>, "a function pointer has an identity");
    static_assert(ComparableEventCallback<std::decay_t<decltype(captureless)>>,
        "a captureless lambda has one: the function it converts to");
    static_assert(!ComparableEventCallback<std::decay_t<decltype(capturing)>>, "a capturing lambda has none");
    static_assert(!ComparableEventCallback<std::function<void(float)>>, "nor has a std::function");
    static_assert(!ComparableEventCallback<int>, "nor has a value that is not callable");
}

int main()
{
    std::cout << "function pointers\n";
    {
        Emitter e;
        e.on("value", &onValueA);
        e.on("value", onValueB);   // a function name decays to the same pointer
        e.on("value", &onValueA);
        e.fire("value", 1.0f);
        check(calls == std::vector<std::string>{"a", "b", "a"}, "all three fire");

        calls.clear();
        e.off("value", onValueA);
        e.fire("value", 1.0f);
        check(calls == std::vector<std::string>{"b"}, "off with a function removes every handler made from it");

        calls.clear();
        e.off("value", &onValueB);
        e.fire("value", 1.0f);
        check(calls.empty() && !e.hasEvent("value"), "and the last one goes too");
    }

    std::cout << "\nthe other forms a handler can take\n";
    {
        Emitter e;
        e.on("raw", &onRaw);
        e.on("raw", HandleEventCallback(&onRaw));
        e.on("raw", &onNothing);
        e.off("raw", &onRaw);
        e.fire("raw");
        check(calls == std::vector<std::string>{"nothing"},
            "a raw-args handler is found whether it came in as a pointer or inside a HandleEventCallback");
        calls.clear();
        e.off("raw", HandleEventCallback(&onRaw));
        e.fire("raw");
        check(calls == std::vector<std::string>{"nothing"},
            "a HandleEventCallback naming a function with no handlers left removes nothing else");
        calls.clear();
        e.off("raw", &onNothing);
        e.fire("raw");
        check(calls.empty(), "the no-argument handler is found by its own pointer");

        e.on("value", captureless);
        e.on("value", &onValueA);
        e.off("value", captureless);
        e.fire("value", 2.0f);
        check(calls == std::vector<std::string>{"a"}, "a captureless lambda is found by the same lambda object");
        calls.clear();

        e.on("value", capturing);
        e.off("value", &onValueA);
        e.fire("value", 2.0f);
        check(calls.empty() && capturedCount == 1, "a handler with no identity is never matched by a function");
    }

    std::cout << "\nonce, scope and every event\n";
    {
        Emitter e;
        int scopeA = 0;
        int scopeB = 0;
        e.once("value", &onValueA);
        e.off("value", &onValueA);
        e.fire("value", 1.0f);
        check(calls.empty(), "a once handler is removed before it fires");

        e.on("value", &onValueA, &scopeA);
        e.on("value", &onValueA, &scopeB);
        e.off("value", &onValueA, &scopeA);
        e.fire("value", 1.0f);
        check(calls == std::vector<std::string>{"a"}, "a scope narrows the removal to that scope's handler");
        calls.clear();

        e.on("other", &onValueA);
        e.on("other", &onValueB);
        e.off("", &onValueA);
        e.fire("value", 1.0f);
        e.fire("other", 1.0f);
        check(calls == std::vector<std::string>{"b"}, "an empty name removes the function from every event");
        calls.clear();

        e.off("other", HandleEventCallback());
        check(!e.hasEvent("other"), "an empty callback still removes every handler of the event");
    }

    std::cout << "\nremoving during dispatch\n";
    {
        Emitter e;
        e.on("value", [&e](const float) { e.off("value", &onValueB); });
        e.on("value", &onValueB);
        e.fire("value", 1.0f);
        check(calls.empty(), "a handler removed by an earlier handler of the same fire does not run");
        e.fire("value", 1.0f);
        check(calls.empty() && e.hasEvent("value"), "and stays removed while the remover stays subscribed");
    }

    return finish("event handler off");
}
