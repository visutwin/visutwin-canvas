// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "animClip.h"

#include <algorithm>

namespace visutwin::canvas
{
    AnimClip::AnimClip(const std::shared_ptr<AnimTrack>& track, const float time, const float speed,
        const bool playing, const bool loop)
        : _track(track), _time(time), _speed(speed), _playing(playing), _loop(loop)
    {
    }

    void AnimClip::reset()
    {
        _time = 0.0f;
        _playing = true;
    }

    void AnimClip::pause()
    {
        _playing = false;
    }

    void AnimClip::resume()
    {
        _playing = true;
    }

    void AnimClip::stop()
    {
        _playing = false;
        _time = 0.0f;
    }

    void AnimClip::update(const float dt)
    {
        if (!_playing || !_track) {
            return;
        }

        _time += dt * _speed;

        const float duration = _track->duration();
        if (duration <= 0.0f) {
            _time = 0.0f;
            return;
        }

        if (_loop) {
            while (_time > duration) {
                _time -= duration;
            }
            while (_time < 0.0f) {
                _time += duration;
            }
        } else {
            _time = std::clamp(_time, 0.0f, duration);
            if (_time == 0.0f || _time == duration) {
                _playing = false;
            }
        }
    }

    void AnimClip::eval(std::unordered_map<std::string, AnimTransform>& transforms) const
    {
        if (!_track) {
            return;
        }

        _track->eval(_time, transforms);
    }
}
