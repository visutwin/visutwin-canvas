// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "framework/anim/evaluator/animEvaluator.h"
#include "animConstants.h"
#include "animState.h"
#include "animTransition.h"

namespace visutwin::canvas
{
    /** Declarative state description used to construct an AnimController. */
    struct AnimStateDesc
    {
        std::string name;
        float speed = 1.0f;
        bool loop = true;
        std::optional<AnimBlendTreeDesc> blendTree;
    };

    /** Declarative transition description used to construct an AnimController. */
    struct AnimTransitionDesc
    {
        std::string from;
        std::string to;
        float time = 0.0f;
        int priority = 0;
        std::vector<AnimCondition> conditions;
        float exitTime = -1.0f;           // normalized; negative = none
        float transitionOffset = -1.0f;   // normalized; negative = none
        AnimInterruption interruptionSource = AnimInterruption::NONE;
    };

    /**
     * The AnimController manages the animations of its entity based on the provided state graph
     * and parameters. Its update method determines the active state from the current time,
     * parameters and available states/transitions, and keeps the AnimEvaluator supplied with the
     * correct clips and blend weights.
     *
     * DEVIATION: animation/transition events are not fired (no EventHandler plumbed through yet).
     */
    class AnimController
    {
    public:
        using FindParameterFn = std::function<AnimParameter*(const std::string&)>;
        using ConsumeTriggerFn = std::function<void(const std::string&)>;

        AnimController(AnimEvaluator* animEvaluator, const std::vector<AnimStateDesc>& states,
                       const std::vector<AnimTransitionDesc>& transitions, bool activate,
                       FindParameterFn findParameter, ConsumeTriggerFn consumeTrigger);

        AnimEvaluator* animEvaluator() const { return _animEvaluator; }

        AnimState* activeState() const { return findState(_activeStateName); }
        const std::string& activeStateName() const { return _activeStateName; }
        AnimState* previousState() const
        {
            return _previousStateName ? findState(*_previousStateName) : nullptr;
        }

        bool playable() const;
        bool playing() const { return _playing; }
        void setPlaying(const bool value) { _playing = value; }

        float activeStateProgress() const { return getActiveStateProgressForTime(_timeInState); }
        float activeStateDuration() const;
        float activeStateCurrentTime() const { return _timeInState; }
        void setActiveStateCurrentTime(float time);

        bool transitioning() const { return _isTransitioning; }
        float transitionProgress() const
        {
            return _totalTransitionTime != 0.0f ? _currTransitionTime / _totalTransitionTime : 1.0f;
        }

        const std::vector<std::string>& states() const { return _stateNames; }

        /**
         * Assign an animation track to a state (or blend-tree leaf). `path` is the state name,
         * optionally followed by blend-tree node names separated by '.'.
         * A missing simple state is created on the fly (matching upstream).
         */
        void assignAnimation(const std::string& path, const std::shared_ptr<AnimTrack>& track,
                             std::optional<float> speed = std::nullopt,
                             std::optional<bool> loop = std::nullopt);

        bool removeNodeAnimations(const std::string& nodeName);

        void play(const std::string& stateName = {});
        void pause();
        void reset();
        void update(float dt);

        const AnimParameter* findParameter(const std::string& name) const
        {
            return _findParameter ? _findParameter(name) : nullptr;
        }

    private:
        AnimState* findState(const std::string& stateName) const;
        float getActiveStateProgressForTime(float time) const;
        const std::vector<AnimTransition*>& findTransitionsFromState(const std::string& stateName);
        const std::vector<AnimTransition*>& findTransitionsBetweenStates(
            const std::string& sourceStateName, const std::string& destinationStateName);
        bool transitionHasConditionsMet(const AnimTransition& transition) const;
        AnimTransition* findTransition(const std::string& from, const std::string& to = {});
        void updateStateFromTransition(const AnimTransition& transition);
        void transitionToState(const std::string& newStateName);

        struct PreviousStateEntry
        {
            std::string name;
            float weight = 1.0f;
        };

        AnimEvaluator* _animEvaluator;
        FindParameterFn _findParameter;
        ConsumeTriggerFn _consumeTrigger;

        std::unordered_map<std::string, std::unique_ptr<AnimState>> _states;
        std::vector<std::string> _stateNames;
        std::vector<std::unique_ptr<AnimTransition>> _transitions;

        std::unordered_map<std::string, std::vector<AnimTransition*>> _findTransitionsFromStateCache;
        std::unordered_map<std::string, std::vector<AnimTransition*>> _findTransitionsBetweenStatesCache;

        std::optional<std::string> _previousStateName;
        std::string _activeStateName = ANIM_STATE_START;
        mutable float _activeStateDuration = 0.0f;
        mutable bool _activeStateDurationDirty = true;

        bool _playing = false;
        bool _activate = true;

        float _currTransitionTime = 1.0f;
        float _totalTransitionTime = 1.0f;
        bool _isTransitioning = false;
        AnimInterruption _transitionInterruptionSource = AnimInterruption::NONE;
        std::vector<PreviousStateEntry> _transitionPreviousStates;

        float _timeInState = 0.0f;
        float _timeInStateBefore = 0.0f;
    };
}
