// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "animController.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    namespace
    {
        std::vector<std::string> splitPath(const std::string& pathString)
        {
            std::vector<std::string> path;
            std::stringstream stream(pathString);
            std::string segment;
            while (std::getline(stream, segment, '.')) {
                path.push_back(segment);
            }
            return path;
        }

        // Transitions sort ascending by priority; stable to preserve authoring order.
        void sortByPriority(std::vector<AnimTransition*>& transitions)
        {
            std::stable_sort(transitions.begin(), transitions.end(),
                [](const AnimTransition* a, const AnimTransition* b) {
                    return a->priority() < b->priority();
                });
        }
    }

    AnimController::AnimController(AnimEvaluator* animEvaluator,
        const std::vector<AnimStateDesc>& states, const std::vector<AnimTransitionDesc>& transitions,
        const bool activate, FindParameterFn findParameter, ConsumeTriggerFn consumeTrigger)
        : _animEvaluator(animEvaluator),
          _findParameter(std::move(findParameter)),
          _consumeTrigger(std::move(consumeTrigger)),
          _activate(activate)
    {
        for (const auto& stateDesc : states) {
            _states[stateDesc.name] = std::make_unique<AnimState>(
                this, stateDesc.name, stateDesc.speed, stateDesc.loop, stateDesc.blendTree);
            _stateNames.push_back(stateDesc.name);
        }
        for (const auto& transitionDesc : transitions) {
            _transitions.push_back(std::make_unique<AnimTransition>(
                transitionDesc.from, transitionDesc.to, transitionDesc.time, transitionDesc.priority,
                transitionDesc.conditions, transitionDesc.exitTime, transitionDesc.transitionOffset,
                transitionDesc.interruptionSource));
        }
    }

    AnimState* AnimController::findState(const std::string& stateName) const
    {
        const auto it = _states.find(stateName);
        return it != _states.end() ? it->second.get() : nullptr;
    }

    bool AnimController::playable() const
    {
        for (const auto& stateName : _stateNames) {
            const auto it = _states.find(stateName);
            if (it != _states.end() && !it->second->playable()) {
                return false;
            }
        }
        return true;
    }

    float AnimController::activeStateDuration() const
    {
        if (_activeStateDurationDirty) {
            float maxDuration = 0.0f;
            if (const AnimState* state = activeState()) {
                for (const auto* animation : state->animations()) {
                    if (animation && animation->animTrack()) {
                        maxDuration = std::max(maxDuration, animation->animTrack()->duration());
                    }
                }
            }
            _activeStateDuration = maxDuration;
            _activeStateDurationDirty = false;
        }
        return _activeStateDuration;
    }

    void AnimController::setActiveStateCurrentTime(const float time)
    {
        _timeInStateBefore = time;
        _timeInState = time;
        if (const AnimState* state = activeState()) {
            for (const auto* animation : state->animations()) {
                if (!animation) continue;
                if (AnimClip* clip = _animEvaluator->findClip(animation->name())) {
                    clip->setTime(time);
                }
            }
        }
    }

    float AnimController::getActiveStateProgressForTime(const float time) const
    {
        if (isAnimControlState(_activeStateName)) {
            return 1.0f;
        }
        const AnimState* state = activeState();
        if (state && !state->animations().empty()) {
            const auto* animation = state->animations()[0];
            if (animation) {
                if (const AnimClip* clip = _animEvaluator->findClip(animation->name())) {
                    return clip->progressForTime(time);
                }
            }
        }
        return 0.0f;
    }

    const std::vector<AnimTransition*>& AnimController::findTransitionsFromState(const std::string& stateName)
    {
        auto it = _findTransitionsFromStateCache.find(stateName);
        if (it == _findTransitionsFromStateCache.end()) {
            std::vector<AnimTransition*> transitions;
            for (const auto& transition : _transitions) {
                if (transition->from() == stateName) {
                    transitions.push_back(transition.get());
                }
            }
            sortByPriority(transitions);
            it = _findTransitionsFromStateCache.emplace(stateName, std::move(transitions)).first;
        }
        return it->second;
    }

    const std::vector<AnimTransition*>& AnimController::findTransitionsBetweenStates(
        const std::string& sourceStateName, const std::string& destinationStateName)
    {
        const std::string key = sourceStateName + "->" + destinationStateName;
        auto it = _findTransitionsBetweenStatesCache.find(key);
        if (it == _findTransitionsBetweenStatesCache.end()) {
            std::vector<AnimTransition*> transitions;
            for (const auto& transition : _transitions) {
                if (transition->from() == sourceStateName && transition->to() == destinationStateName) {
                    transitions.push_back(transition.get());
                }
            }
            sortByPriority(transitions);
            it = _findTransitionsBetweenStatesCache.emplace(key, std::move(transitions)).first;
        }
        return it->second;
    }

    bool AnimController::transitionHasConditionsMet(const AnimTransition& transition) const
    {
        for (const auto& condition : transition.conditions()) {
            const AnimParameter* parameter = findParameter(condition.parameterName);
            if (!parameter) {
                return false;
            }
            const float value = parameter->value;
            switch (condition.predicate) {
                case AnimPredicate::GREATER_THAN:
                    if (!(value > condition.value)) return false;
                    break;
                case AnimPredicate::LESS_THAN:
                    if (!(value < condition.value)) return false;
                    break;
                case AnimPredicate::GREATER_THAN_EQUAL_TO:
                    if (!(value >= condition.value)) return false;
                    break;
                case AnimPredicate::LESS_THAN_EQUAL_TO:
                    if (!(value <= condition.value)) return false;
                    break;
                case AnimPredicate::EQUAL_TO:
                    if (!(value == condition.value)) return false;
                    break;
                case AnimPredicate::NOT_EQUAL_TO:
                    if (!(value != condition.value)) return false;
                    break;
            }
        }
        return true;
    }

    AnimTransition* AnimController::findTransition(const std::string& from, const std::string& to)
    {
        std::vector<AnimTransition*> transitions;
        const auto append = [&transitions](const std::vector<AnimTransition*>& source) {
            transitions.insert(transitions.end(), source.begin(), source.end());
        };

        if (!from.empty() && !to.empty()) {
            append(findTransitionsBetweenStates(from, to));
        } else if (!_isTransitioning) {
            append(findTransitionsFromState(_activeStateName));
            append(findTransitionsFromState(ANIM_STATE_ANY));
        } else {
            switch (_transitionInterruptionSource) {
                case AnimInterruption::PREV:
                    if (_previousStateName) append(findTransitionsFromState(*_previousStateName));
                    append(findTransitionsFromState(ANIM_STATE_ANY));
                    break;
                case AnimInterruption::NEXT:
                    append(findTransitionsFromState(_activeStateName));
                    append(findTransitionsFromState(ANIM_STATE_ANY));
                    break;
                case AnimInterruption::PREV_NEXT:
                    if (_previousStateName) append(findTransitionsFromState(*_previousStateName));
                    append(findTransitionsFromState(_activeStateName));
                    append(findTransitionsFromState(ANIM_STATE_ANY));
                    break;
                case AnimInterruption::NEXT_PREV:
                    append(findTransitionsFromState(_activeStateName));
                    if (_previousStateName) append(findTransitionsFromState(*_previousStateName));
                    append(findTransitionsFromState(ANIM_STATE_ANY));
                    break;
                case AnimInterruption::NONE:
                default:
                    break;
            }
        }

        // Filter to transitions whose exit time and conditions are met.
        std::vector<AnimTransition*> valid;
        for (auto* transition : transitions) {
            // if the transition is moving to the already active state, ignore it
            if (transition->to() == _activeStateName) {
                continue;
            }
            // when an exit time is present, only exit if it falls within the frame's delta time
            if (transition->hasExitTime()) {
                float progressBefore = getActiveStateProgressForTime(_timeInStateBefore);
                float progress = getActiveStateProgressForTime(_timeInState);
                // exit times < 1 on looping states are checked every loop
                const AnimState* state = activeState();
                if (transition->exitTime() < 1.0f && state && state->loop()) {
                    progressBefore -= std::floor(progressBefore);
                    progress -= std::floor(progress);
                }
                if (progress == progressBefore) {
                    if (progress != transition->exitTime()) {
                        continue;
                    }
                } else if (!(transition->exitTime() > progressBefore && transition->exitTime() <= progress)) {
                    continue;
                }
            }
            if (transitionHasConditionsMet(*transition)) {
                valid.push_back(transition);
            }
        }

        if (!valid.empty()) {
            AnimTransition* transition = valid[0];
            if (transition->to() == ANIM_STATE_END) {
                const auto& startTransitions = findTransitionsFromState(ANIM_STATE_START);
                if (!startTransitions.empty()) {
                    transition->setTo(startTransitions[0]->to());
                }
            }
            return transition;
        }
        return nullptr;
    }

    void AnimController::updateStateFromTransition(const AnimTransition& transition)
    {
        // If transition.from is set, we transition away from the current active state
        // (which could be the previous, active or ANY state). Otherwise clear previousState.
        if (!transition.from().empty()) {
            _previousStateName = _activeStateName;
        } else {
            _previousStateName.reset();
        }
        _activeStateName = transition.to();
        _activeStateDurationDirty = true;

        // Consume any triggers that were required to activate this transition.
        for (const auto& condition : transition.conditions()) {
            const AnimParameter* parameter = findParameter(condition.parameterName);
            if (parameter && parameter->type == AnimParameterType::TRIGGER && _consumeTrigger) {
                _consumeTrigger(condition.parameterName);
            }
        }

        if (_previousStateName) {
            if (!_isTransitioning) {
                _transitionPreviousStates.clear();
            }

            _transitionPreviousStates.push_back({*_previousStateName, 1.0f});

            // If this transition interrupts another, redistribute previous-state weights
            // based on the progress through the interrupted transition.
            const float interpolatedTime = std::min(
                _totalTransitionTime != 0.0f ? _currTransitionTime / _totalTransitionTime : 1.0f, 1.0f);
            for (size_t i = 0; i < _transitionPreviousStates.size(); ++i) {
                if (!_isTransitioning) {
                    _transitionPreviousStates[i].weight = 1.0f;
                } else if (i != _transitionPreviousStates.size() - 1) {
                    _transitionPreviousStates[i].weight *= (1.0f - interpolatedTime);
                } else {
                    _transitionPreviousStates[i].weight = interpolatedTime;
                }
                const AnimState* state = findState(_transitionPreviousStates[i].name);
                if (!state) continue;
                // Rename previous-state clips to include their position in the previous
                // states array, uniquely identifying clips added over multiple transitions.
                for (const auto* animation : state->animations()) {
                    if (!animation) continue;
                    const std::string previousName =
                        animation->name() + ".previous." + std::to_string(i);
                    AnimClip* clip = _animEvaluator->findClip(previousName);
                    if (!clip) {
                        clip = _animEvaluator->findClip(animation->name());
                        if (clip) {
                            clip->setName(previousName);
                        }
                    }
                    // Pause all but the most recent previous-state clips to reduce cost.
                    if (clip && i != _transitionPreviousStates.size() - 1) {
                        clip->pause();
                    }
                }
            }
        }

        _isTransitioning = true;
        _totalTransitionTime = transition.time();
        _currTransitionTime = 0.0f;
        _transitionInterruptionSource = transition.interruptionSource();

        AnimState* newActiveState = activeState();
        if (!newActiveState) {
            return;
        }
        const bool hasTransitionOffset = transition.transitionOffset() > 0.0f &&
            transition.transitionOffset() < 1.0f;

        float timeInState = 0.0f;
        if (hasTransitionOffset) {
            timeInState = newActiveState->timelineDuration() * transition.transitionOffset();
        }
        _timeInState = timeInState;
        _timeInStateBefore = timeInState;

        // Add clips to the evaluator for each animation in the new state.
        for (const auto* animation : newActiveState->animations()) {
            if (!animation) continue;
            AnimClip* clip = _animEvaluator->findClip(animation->name());
            if (!clip) {
                auto newClip = std::make_shared<AnimClip>(animation->animTrack(), _timeInState,
                    animation->speed(), true, newActiveState->loop());
                newClip->setName(animation->name());
                _animEvaluator->addClip(newClip);
                clip = newClip.get();
            } else {
                clip->reset();
            }
            if (transition.time() > 0.0f) {
                clip->setBlendWeight(0.0f);
            } else {
                clip->setBlendWeight(animation->normalizedWeight());
            }
            clip->play();
            if (hasTransitionOffset) {
                clip->setTime(newActiveState->timelineDuration() * transition.transitionOffset());
            } else {
                clip->setTime(newActiveState->speed() >= 0.0f ? 0.0f : activeStateDuration());
            }
        }
    }

    void AnimController::transitionToState(const std::string& newStateName)
    {
        if (!findState(newStateName)) {
            return;
        }
        // Use a graph transition when one exists; otherwise move instantly.
        AnimTransition* transition = findTransition(_activeStateName, newStateName);
        if (!transition) {
            _animEvaluator->removeClips();
            const AnimTransition instant({}, newStateName);
            updateStateFromTransition(instant);
        } else {
            updateStateFromTransition(*transition);
        }
    }

    void AnimController::assignAnimation(const std::string& path,
        const std::shared_ptr<AnimTrack>& track, const std::optional<float> speed,
        const std::optional<bool> loop)
    {
        const auto pathSegments = splitPath(path);
        if (pathSegments.empty()) {
            return;
        }
        AnimState* state = findState(pathSegments[0]);
        if (!state) {
            _states[pathSegments[0]] = std::make_unique<AnimState>(
                this, pathSegments[0], speed.value_or(1.0f));
            _stateNames.push_back(pathSegments[0]);
            state = _states[pathSegments[0]].get();
        }
        state->addAnimation(pathSegments, track);
        _animEvaluator->updateClipTrack(state->name(), track);
        if (speed.has_value()) {
            state->setSpeed(*speed);
        }
        if (loop.has_value()) {
            state->setLoop(*loop);
        }

        if (!_playing && _activate && playable()) {
            play();
        }
        _activeStateDurationDirty = true;
    }

    bool AnimController::removeNodeAnimations(const std::string& nodeName)
    {
        if (isAnimControlState(nodeName)) {
            return false;
        }
        AnimState* state = findState(nodeName);
        if (!state) {
            spdlog::error("Attempting to unassign animation tracks from a state that does not exist: {}", nodeName);
            return false;
        }
        state->clearAnimations();
        return true;
    }

    void AnimController::play(const std::string& stateName)
    {
        if (!stateName.empty()) {
            transitionToState(stateName);
        }
        _playing = true;
    }

    void AnimController::pause()
    {
        _playing = false;
    }

    void AnimController::reset()
    {
        _previousStateName.reset();
        _activeStateName = ANIM_STATE_START;
        _playing = false;
        _currTransitionTime = 1.0f;
        _totalTransitionTime = 1.0f;
        _isTransitioning = false;
        _timeInState = 0.0f;
        _timeInStateBefore = 0.0f;
        _animEvaluator->removeClips();
    }

    void AnimController::update(float dt)
    {
        if (!_playing) {
            return;
        }

        AnimState* state = activeState();
        if (!state) {
            return;
        }

        // Advance time when looping or before the active state's end.
        if (state->loop() || _timeInState < activeStateDuration()) {
            _timeInStateBefore = _timeInState;
            _timeInState += dt * state->speed();
            if (!state->loop() && _timeInState > activeStateDuration()) {
                _timeInState = activeStateDuration();
                dt = activeStateDuration() - _timeInStateBefore;
            }
        }

        // Transition between states when one is available from the active state.
        if (const AnimTransition* transition = findTransition(_activeStateName)) {
            updateStateFromTransition(*transition);
            state = activeState();
        }

        if (_isTransitioning) {
            _currTransitionTime += dt;
            if (_currTransitionTime <= _totalTransitionTime) {
                const float interpolatedTime = _totalTransitionTime != 0.0f
                    ? _currTransitionTime / _totalTransitionTime : 1.0f;
                // Weight previous-state clips by (1 - t) and the active state's by t.
                for (size_t i = 0; i < _transitionPreviousStates.size(); ++i) {
                    const AnimState* previousState = findState(_transitionPreviousStates[i].name);
                    if (!previousState) continue;
                    const float stateWeight = _transitionPreviousStates[i].weight;
                    for (const auto* animation : previousState->animations()) {
                        if (!animation) continue;
                        if (AnimClip* clip = _animEvaluator->findClip(
                                animation->name() + ".previous." + std::to_string(i))) {
                            clip->setBlendWeight(
                                (1.0f - interpolatedTime) * animation->normalizedWeight() * stateWeight);
                        }
                    }
                }
                if (state) {
                    for (const auto* animation : state->animations()) {
                        if (!animation) continue;
                        if (AnimClip* clip = _animEvaluator->findClip(animation->name())) {
                            clip->setBlendWeight(interpolatedTime * animation->normalizedWeight());
                        }
                    }
                }
            } else {
                _isTransitioning = false;
                // Remove all previous-state clips; active-state clips are at the end.
                if (state) {
                    const size_t activeClips = state->animations().size();
                    const size_t totalClips = _animEvaluator->clips().size();
                    for (size_t i = 0; i + activeClips < totalClips; ++i) {
                        _animEvaluator->removeClip(0);
                    }
                    _transitionPreviousStates.clear();
                    for (const auto* animation : state->animations()) {
                        if (!animation) continue;
                        if (AnimClip* clip = _animEvaluator->findClip(animation->name())) {
                            clip->setBlendWeight(animation->normalizedWeight());
                        }
                    }
                }
            }
        } else if (state && state->blendTree() && state->blendTree()->isBlendTree()) {
            // Blend-tree states refresh their child weights (and speeds when synced) each frame.
            for (const auto* animation : state->animations()) {
                if (!animation) continue;
                if (AnimClip* clip = _animEvaluator->findClip(animation->name())) {
                    clip->setBlendWeight(animation->normalizedWeight());
                    if (animation->parent() && animation->parent()->syncAnimations()) {
                        clip->setSpeed(animation->speed());
                    }
                }
            }
        }

        _animEvaluator->update(dt);
    }
}
