// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// EventHandler::fire nested inside a handler of the same event, and a handler that throws.
// A once handler is consumed BEFORE it is called, so it runs at most once even when it fires
// its own event again; a nested fire neither repeats a handler the outer one is still to reach
// nor clears the list the outer one iterates; a handler added during a fire takes part in the
// fires that start after it; and a throwing handler leaves the handler able to fire again,
// with no once handler left subscribed and no stale list behind. The cases are upstream's own;
// the two about a handler removed DURING a fire are written for this port's rule (it is
// skipped; upstream still calls it).

#include <iostream>
#include <stdexcept>
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
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "once\n";
    {
        Emitter e;
        int count = 0;
        const auto handle = e.once("test", [&] {
            ++count;
            if (count == 1) {
                e.fire("test");
            }
        });
        e.fire("test");
        check(count == 1 && handle->removed() && !e.hasEvent("test"),
            "a once handler firing its own event runs once and is gone");
    }
    {
        Emitter e;
        std::vector<std::string> calls;
        e.on("test", [&](const std::string& value) {
            calls.push_back("on:" + value);
            if (value == "outer") {
                e.fire("test", std::string("inner"));
            }
        });
        e.once("test", [&](const std::string& value) { calls.push_back("once:" + value); });
        e.fire("test", std::string("outer"));
        check(calls == std::vector<std::string>{"on:outer", "on:inner", "once:inner"},
            "a once handler consumed by a nested fire is not repeated by the outer one");
    }
    {
        Emitter e;
        std::vector<std::string> calls;
        e.once("test", [&](const std::string& value) {
            calls.push_back("a:" + value);
            if (value == "outer") {
                e.fire("test", std::string("inner"));
            }
        });
        e.once("test", [&](const std::string& value) { calls.push_back("b:" + value); });
        e.fire("test", std::string("outer"));
        check(calls == std::vector<std::string>{"a:outer", "b:inner"} && !e.hasEvent("test"),
            "each once handler runs once across nested fires");
    }
    {
        Emitter e;
        int count = 0;
        const auto handle = e.once("test", [&] {
            ++count;
            throw std::runtime_error("listener failed");
        });
        bool threw = false;
        try {
            e.fire("test");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        e.fire("test");
        check(threw && handle->removed() && !e.hasEvent("test") && count == 1,
            "a once handler is removed even when it throws");
    }

    std::cout << "nested fires\n";
    {
        Emitter e;
        std::vector<std::string> calls;
        e.on("test", [&](const std::string& value) {
            calls.push_back("a:" + value);
            if (value == "outer") {
                e.fire("test", std::string("inner"));
            }
        });
        e.on("test", [&](const std::string& value) { calls.push_back("b:" + value); });
        e.fire("test", std::string("outer"));
        check(calls == std::vector<std::string>{"a:outer", "a:inner", "b:inner", "b:outer"},
            "the outer fire finishes its handlers after a nested one");
    }
    {
        Emitter e;
        std::vector<std::string> calls;
        e.on("test", [&](const std::string& value) {
            calls.push_back("a:" + value);
            if (value == "outer") {
                e.on("test", [&](const std::string& v) { calls.push_back("c:" + v); });
                e.fire("test", std::string("inner"));
            }
        });
        e.on("test", [&](const std::string& value) { calls.push_back("b:" + value); });
        e.fire("test", std::string("outer"));
        check(calls == std::vector<std::string>{"a:outer", "a:inner", "b:inner", "c:inner", "b:outer"},
            "a handler added during a fire takes part in a nested fire, not the outer one");
    }
    {
        Emitter e;
        std::vector<std::string> calls;
        EventHandlePtr handle;
        e.on("test", [&](const std::string& value) {
            calls.push_back("a:" + value);
            if (value == "outer") {
                e.fire("test", std::string("inner"));
            } else if (value == "inner") {
                handle->off();
            }
        });
        handle = e.on("test", [&](const std::string& value) { calls.push_back("b:" + value); });
        e.fire("test", std::string("outer"));
        e.fire("test", std::string("later"));
        check(calls == std::vector<std::string>{"a:outer", "a:inner", "a:later"},
            "a handler removed during a nested fire is called by neither fire (DEVIATION: upstream "
            "still calls it in both)");
    }
    {
        Emitter e;
        std::vector<std::string> calls;
        EventHandlePtr handle;
        e.on("test", [&] {
            calls.push_back("on");
            handle->off();
        });
        handle = e.once("test", [&] { calls.push_back("once"); });
        e.fire("test");
        e.fire("test");
        check(calls == std::vector<std::string>{"on", "on"},
            "a once handler removed during the fire is not called (DEVIATION: upstream calls it)");
    }

    std::cout << "a throwing handler\n";
    {
        Emitter e;
        std::vector<std::string> calls;
        e.on("test", [&] {
            calls.push_back("throw");
            throw std::runtime_error("listener failed");
        });
        e.on("test", [&] { calls.push_back("old"); });
        try {
            e.fire("test");
        } catch (const std::runtime_error&) {
        }
        e.off("test");
        e.on("test", [&] { calls.push_back("new"); });
        e.fire("test");
        check(calls == std::vector<std::string>{"throw", "new"},
            "after a handler throws, the next fire starts from the handlers then subscribed");
    }

    return finish("event handler dispatch");
}
