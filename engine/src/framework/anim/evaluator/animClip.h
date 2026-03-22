// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "animTrack.h"

namespace visutwin::canvas
{
    class AnimClip
    {
    public:
        AnimClip(const std::shared_ptr<AnimTrack>& track, float time, float speed, bool playing, bool loop);

        void reset();
        void pause();
        void resume();
        void stop();
        void update(float dt);

        void eval(std::unordered_map<std::string, AnimTransform>& transforms) const;

        float time() const { return _time; }
        void setTime(float value) { _time = value; }

        float speed() const { return _speed; }
        void setSpeed(float value) { _speed = value; }

        bool loop() const { return _loop; }
        void setLoop(bool value) { _loop = value; }

        bool playing() const { return _playing; }

        float blendWeight() const { return _blendWeight; }
        void setBlendWeight(float value) { _blendWeight = value; }

        const std::string& name() const { return _name; }
        void setName(const std::string& value) { _name = value; }

        const std::shared_ptr<AnimTrack>& track() const { return _track; }

    private:
        std::shared_ptr<AnimTrack> _track;

        float _time = 0.0f;
        float _speed = 1.0f;
        bool _playing = true;
        bool _loop = true;
        float _blendWeight = 1.0f;

        std::string _name;
    };
}
