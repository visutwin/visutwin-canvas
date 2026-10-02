// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.01.2026
//
// Input framework: deltas that accumulate between
// reads, frames that group them by name, sources that fill a frame from a device, and
// controllers that turn a frame of `move` and `rotate` deltas into a Pose.
//
// DEVIATION: a source attaches to the Engine (whose input devices it listens to) rather
// than to a DOM element; the devices are fed by the application's event loop.
//
#pragma once

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "core/eventHandler.h"
#include "pose.h"

namespace visutwin::canvas
{
    class Engine;

    /// A fixed-length array that sources append to and read() flushes.
    class InputDelta
    {
    public:
        explicit InputDelta(const size_t size = 0) : _value(size, 0.0f) {}
        explicit InputDelta(std::vector<float> initial) : _value(std::move(initial)) {}

        InputDelta& add(const InputDelta& other)
        {
            for (size_t i = 0; i < _value.size(); ++i) {
                _value[i] += i < other._value.size() ? other._value[i] : 0.0f;
            }
            return *this;
        }

        InputDelta& append(const std::vector<float>& offsets)
        {
            for (size_t i = 0; i < _value.size(); ++i) {
                _value[i] += i < offsets.size() ? offsets[i] : 0.0f;
            }
            return *this;
        }

        InputDelta& copy(const InputDelta& other)
        {
            for (size_t i = 0; i < _value.size(); ++i) {
                _value[i] = i < other._value.size() ? other._value[i] : 0.0f;
            }
            return *this;
        }

        [[nodiscard]] float length() const
        {
            float sum = 0.0f;
            for (const float v : _value) {
                sum += v * v;
            }
            return std::sqrt(sum);
        }

        /// The accumulated value, after which the delta is zero again.
        std::vector<float> read()
        {
            std::vector<float> value = _value;
            std::fill(_value.begin(), _value.end(), 0.0f);
            return value;
        }

        /// Direct access, for a source that overwrites a lane (gamepad buttons).
        float& operator[](const size_t i) { return _value[i]; }
        [[nodiscard]] size_t size() const { return _value.size(); }

    private:
        std::vector<float> _value;
    };

    /// The values one InputFrame::read() returns, by delta name.
    using InputValues = std::map<std::string, std::vector<float>>;

    /// Named deltas, their names and sizes fixed at construction.
    class InputFrame
    {
    public:
        InputFrame() = default;
        explicit InputFrame(const std::map<std::string, size_t>& shape)
        {
            for (const auto& [name, size] : shape) {
                deltas.emplace(name, InputDelta(size));
            }
        }
        virtual ~InputFrame() = default;

        /// Every delta's value, each reset to zero.
        virtual InputValues read()
        {
            InputValues values;
            for (auto& [name, delta] : deltas) {
                values.emplace(name, delta.read());
            }
            return values;
        }

        InputDelta& delta(const std::string& name) { return deltas.at(name); }

        std::map<std::string, InputDelta> deltas;
    };

    /// An InputFrame a device fills. attach() starts listening and
    /// detach() stops (flushing what was accumulated).
    class InputSource : public InputFrame, public EventHandler
    {
    public:
        using InputFrame::InputFrame;
        ~InputSource() override = default;

        virtual void attach(Engine* engine)
        {
            if (_engine) {
                detach();
            }
            _engine = engine;
        }

        virtual void detach()
        {
            if (!_engine) {
                return;
            }
            _engine = nullptr;
            read();
        }

        void destroy()
        {
            detach();
            off();
        }

    protected:
        Engine* _engine = nullptr;
    };

    /// Does something with a frame each update (by default, discards it).
    class InputConsumer
    {
    public:
        virtual ~InputConsumer() = default;

        virtual void update(InputFrame& frame, float /*dt*/) { frame.read(); }
    };

    /// Consumes a frame of `move` and `rotate` deltas and produces a Pose.
    /// attach() sets where it starts, detach() releases it.
    class InputController : public InputConsumer
    {
    public:
        virtual void attach(const Pose& /*pose*/, bool /*smooth*/ = true) {}
        virtual void detach() {}

        /// Applies the frame and returns the controller's pose.
        virtual const Pose& updatePose(InputFrame& frame, float dt)
        {
            InputConsumer::update(frame, dt);
            return _pose;
        }

        void update(InputFrame& frame, const float dt) override { updatePose(frame, dt); }

    protected:
        Pose _pose;
    };
}
