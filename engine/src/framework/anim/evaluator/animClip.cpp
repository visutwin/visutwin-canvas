// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "animClip.h"

#include <algorithm>
#include <cmath>

#include "core/eventHandler.h"

namespace visutwin::canvas
{
    AnimClip::AnimClip(const std::shared_ptr<AnimTrack>& track, const float time, const float speed,
        const bool playing, const bool loop, EventHandler* eventHandler)
        : _track(track), _time(time), _speed(speed), _playing(playing), _loop(loop), _eventHandler(eventHandler)
    {
        alignCursorToCurrentTime();
    }

    void AnimClip::setTime(const float value)
    {
        _time = value;
        alignCursorToCurrentTime();
    }

    void AnimClip::setSpeed(const float value)
    {
        const bool signChanged = (value < 0.0f) != (_speed < 0.0f);
        _speed = value;
        if (signChanged) {
            alignCursorToCurrentTime();
        }
    }

    void AnimClip::reset()
    {
        _time = 0.0f;
        _playing = true;
        alignCursorToCurrentTime();
    }

    // --- Events (upstream anim-clip.js, line for line) ---

    const AnimEvent* AnimClip::nextEvent() const
    {
        if (!_track || _eventCursor < 0 || _eventCursor >= static_cast<int>(_track->events().size())) {
            return nullptr;
        }
        return &_track->events()[static_cast<size_t>(_eventCursor)];
    }

    int AnimClip::eventCursorEnd() const
    {
        return isReverse() ? 0 : static_cast<int>(_track->events().size()) - 1;
    }

    bool AnimClip::nextEventAheadOfTime(const float time) const
    {
        const AnimEvent* event = nextEvent();
        if (!event) return false;
        return isReverse() ? event->time <= time : event->time >= time;
    }

    bool AnimClip::nextEventBehindTime(const float time) const
    {
        const AnimEvent* event = nextEvent();
        if (!event) return false;
        if (time == _track->duration()) {
            return isReverse() ? event->time >= time : event->time <= time;
        }
        return isReverse() ? event->time > time : event->time < time;
    }

    void AnimClip::resetEventCursor()
    {
        _eventCursor = (isReverse() && _track) ? static_cast<int>(_track->events().size()) - 1 : 0;
    }

    void AnimClip::moveEventCursor()
    {
        const int count = static_cast<int>(_track->events().size());
        _eventCursor += isReverse() ? -1 : 1;
        if (_eventCursor >= count) {
            _eventCursor = 0;
        } else if (_eventCursor < 0) {
            _eventCursor = count - 1;
        }
    }

    void AnimClip::alignCursorToCurrentTime()
    {
        resetEventCursor();
        if (!_track) {
            return;
        }
        while (nextEventBehindTime(_time) && _eventCursor != eventCursorEnd()) {
            moveEventCursor();
        }
    }

    void AnimClip::fireNextEvent()
    {
        if (_eventHandler) {
            const AnimEvent* event = nextEvent();
            _eventHandler->fire(event->name, AnimEventFired{_track.get(), event});
        }
        moveEventCursor();
    }

    bool AnimClip::fireNextEventInFrame(const float frameStartTime, const float frameEndTime)
    {
        if (nextEventAheadOfTime(frameStartTime) && nextEventBehindTime(frameEndTime)) {
            fireNextEvent();
            return true;
        }
        return false;
    }

    void AnimClip::activeEventsForFrame(const float frameStartTime, const float frameEndTime)
    {
        // Clip the frame to the track, keeping what runs past its end as the residual.
        const float duration = _track->duration();
        float start = 0.0f;
        float end = frameEndTime;
        float residual = 0.0f;
        if (isReverse()) {
            if (frameEndTime < 0.0f) {
                start = duration;
                end = 0.0f;
                residual = frameEndTime + duration;
            }
        } else if (frameEndTime > duration) {
            start = 0.0f;
            end = duration;
            residual = frameEndTime - duration;
        }

        // Fire every event that falls in the clipped frame, once round at most.
        const int initialCursor = _eventCursor;
        while (fireNextEventInFrame(frameStartTime, end)) {
            if (initialCursor == _eventCursor) {
                break;
            }
        }
        // Then the part of the frame that looped past the end, until nothing is left.
        if (_loop && std::fabs(residual) > 0.0f) {
            activeEventsForFrame(start, residual);
        }
    }

    void AnimClip::pause()
    {
        _playing = false;
    }

    void AnimClip::resume()
    {
        _playing = true;
    }

    void AnimClip::play()
    {
        _playing = true;
    }

    float AnimClip::progressForTime(const float time) const
    {
        const float duration = _track ? _track->duration() : 0.0f;
        if (duration <= 0.0f) {
            return 1.0f;
        }
        return (time * _speed) / duration;
    }

    void AnimClip::stop()
    {
        _playing = false;
        _time = 0.0f;
        alignCursorToCurrentTime();
    }

    void AnimClip::update(const float dt)
    {
        if (!_playing || !_track) {
            return;
        }

        // Events that fire during this frame, before the time moves.
        if (!_track->events().empty() && _track->duration() > 0.0f) {
            activeEventsForFrame(_time, _time + _speed * dt);
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

    void AnimClip::eval(AnimTransform* out, uint8_t* touched) const
    {
        if (!_track) {
            return;
        }

        _track->eval(_time, out, touched);
    }
}
