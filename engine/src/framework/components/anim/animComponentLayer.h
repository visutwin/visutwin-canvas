// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "framework/anim/controller/animController.h"
#include "framework/anim/evaluator/animEvaluator.h"

namespace visutwin::canvas
{
    class AnimComponent;

    /**
     * A layer of an AnimComponent: an independent state machine (AnimController) with its
     * own AnimEvaluator, driving the entity hierarchy while the layer's weight is > 0.
     *
     * DEVIATION: no per-target cross-layer blending or bone masks yet (upstream
     * AnimTargetValue) — layers apply in order, later layers win on shared targets.
     */
    class AnimComponentLayer
    {
    public:
        AnimComponentLayer(std::string name, AnimComponent* component,
                           const std::vector<AnimStateDesc>& states,
                           const std::vector<AnimTransitionDesc>& transitions,
                           float weight, bool activate);

        const std::string& name() const { return _name; }

        float weight() const { return _weight; }
        void setWeight(const float value) { _weight = value; }

        AnimController* controller() const { return _controller.get(); }

        /** Start playing, optionally transitioning to the named state first. */
        void play(const std::string& stateName = {}) { _controller->play(stateName); }
        void pause() { _controller->pause(); }
        void reset() { _controller->reset(); }

        /** Transition to the named state (uses a graph transition when one exists). */
        void transition(const std::string& to) { _controller->play(to); }

        bool playing() const { return _controller->playing(); }
        void setPlaying(const bool value) { _controller->setPlaying(value); }
        bool playable() const { return _controller->playable(); }

        const std::string& activeState() const { return _controller->activeStateName(); }
        float activeStateProgress() const { return _controller->activeStateProgress(); }
        float activeStateDuration() const { return _controller->activeStateDuration(); }
        bool transitioning() const { return _controller->transitioning(); }

        void assignAnimation(const std::string& path, const std::shared_ptr<AnimTrack>& track,
                             const std::optional<float> speed = std::nullopt,
                             const std::optional<bool> loop = std::nullopt)
        {
            _controller->assignAnimation(path, track, speed, loop);
        }

        void removeNodeAnimations(const std::string& nodeName)
        {
            _controller->removeNodeAnimations(nodeName);
        }

        void update(const float dt) { _controller->update(dt); }

    private:
        std::string _name;
        AnimComponent* _component;
        float _weight;
        std::unique_ptr<AnimEvaluator> _evaluator;
        std::unique_ptr<AnimController> _controller;
    };
}
