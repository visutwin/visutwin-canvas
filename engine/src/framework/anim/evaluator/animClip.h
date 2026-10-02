// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "animTrack.h"

namespace visutwin::canvas
{
    class EventHandler;

    /**
     * The running state of one animation track: the play cursor, looping, and the
     * track's events. An event is fired on `eventHandler` (the AnimComponent, as
     * upstream) when the cursor passes its time, in the direction of play, including
     * the events of every lap a large step loops through (upstream anim-clip.js).
     */
    class AnimClip
    {
    public:
        AnimClip(const std::shared_ptr<AnimTrack>& track, float time, float speed, bool playing, bool loop,
            EventHandler* eventHandler = nullptr);

        void reset();
        void pause();
        void resume();
        void play();
        void stop();
        void update(float dt);

        /** Normalized progress the clip reaches at `time` (upstream progressForTime). */
        float progressForTime(float time) const;

        /// The track at the clip's time; see AnimTrack::eval.
        void eval(AnimTransform* out, uint8_t* touched) const;

        float time() const { return _time; }
        void setTime(float value);

        float speed() const { return _speed; }
        void setSpeed(float value);

        bool loop() const { return _loop; }
        void setLoop(bool value) { _loop = value; }

        bool playing() const { return _playing; }

        float blendWeight() const { return _blendWeight; }
        void setBlendWeight(float value) { _blendWeight = value; }

        const std::string& name() const { return _name; }
        void setName(const std::string& value) { _name = value; }

        const std::shared_ptr<AnimTrack>& track() const { return _track; }
        void setTrack(const std::shared_ptr<AnimTrack>& track) { _track = track; alignCursorToCurrentTime(); }

        EventHandler* eventHandler() const { return _eventHandler; }
        void setEventHandler(EventHandler* value) { _eventHandler = value; }

    private:
        // Upstream's event cursor: the index of the next event in the direction of play.
        bool isReverse() const { return _speed < 0.0f; }
        const AnimEvent* nextEvent() const;
        int eventCursorEnd() const;
        bool nextEventAheadOfTime(float time) const;
        bool nextEventBehindTime(float time) const;
        void resetEventCursor();
        void moveEventCursor();
        void alignCursorToCurrentTime();
        void fireNextEvent();
        bool fireNextEventInFrame(float frameStartTime, float frameEndTime);
        void activeEventsForFrame(float frameStartTime, float frameEndTime);

        std::shared_ptr<AnimTrack> _track;

        float _time = 0.0f;
        float _speed = 1.0f;
        bool _playing = true;
        bool _loop = true;
        float _blendWeight = 1.0f;

        std::string _name;

        EventHandler* _eventHandler = nullptr;
        int _eventCursor = 0;
    };
}
