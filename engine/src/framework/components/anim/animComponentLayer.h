// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.07.2026
//
#pragma once

#include <cstdint>

#include <unordered_set>
#include "framework/anim/state-graph/animStateGraph.h"
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
     * own AnimEvaluator. The layer does not write the hierarchy itself: its evaluator
     * hands the layer's pose to the component, which composes every layer's value for
     * a node by layer weight and blend type and writes once.
     * A weight of 0.25 is therefore a quarter contribution, not an on/off switch, and
     * a mask limits the layer to the listed node paths.
     */
    class AnimComponentLayer
    {
    public:
        AnimComponentLayer(std::string name, size_t index, AnimComponent* component,
                           const std::vector<AnimStateDesc>& states,
                           const std::vector<AnimTransitionDesc>& transitions,
                           float weight, AnimLayerBlendType blendType,
                           std::unordered_set<std::string> mask, bool activate);

        const std::string& name() const { return _name; }

        size_t index() const { return _index; }
        float weight() const { return _weight; }
        void setWeight(const float value) { _weight = value; }
        AnimLayerBlendType blendType() const { return _blendType; }
        void setBlendType(const AnimLayerBlendType value) { _blendType = value; }
        /** Node paths this layer may drive (curve nodeName); empty = every node. */
        const std::unordered_set<std::string>& mask() const { return _mask; }
        void setMask(std::unordered_set<std::string> value) { _mask = std::move(value); ++_maskVersion; }
        /** Node paths the layer drives with their whole subtree (upstream's `{children: true}`),
         *  beside the exact paths of mask(). */
        const std::vector<std::string>& maskSubtrees() const { return _maskSubtrees; }
        void setMaskSubtrees(std::vector<std::string> value) { _maskSubtrees = std::move(value); ++_maskVersion; }
        /** True when the layer has no mask: it drives every node. */
        bool unmasked() const { return _mask.empty() && _maskSubtrees.empty(); }
        bool drives(const std::string& nodePath) const
        {
            if (unmasked() || _mask.contains(nodePath)) {
                return true;
            }
            for (const auto& root : _maskSubtrees) {
                if (nodePath.size() >= root.size() && nodePath.compare(0, root.size(), root) == 0 &&
                    (nodePath.size() == root.size() || nodePath[root.size()] == '/')) {
                    return true;
                }
            }
            return false;
        }
        /// Bumped by setMask, so a caller caching drives() per node knows when to ask again.
        uint64_t maskVersion() const { return _maskVersion; }
        AnimEvaluator* evaluator() const { return _evaluator.get(); }

        AnimController* controller() const { return _controller.get(); }

        /** Start playing, optionally transitioning to the named state first. */
        void play(const std::string& stateName = {}) { _controller->play(stateName); }
        void pause() { _controller->pause(); }
        void reset() { _controller->reset(); }

        /**
         * Blend from the active state to the named one over `time` seconds, whatever the graph's
         * transitions say; a negative offset
         * is none, starting the new state from its beginning.
         */
        void transition(const std::string& to, const float time = 0.0f, const float transitionOffset = -1.0f)
        {
            _controller->transition(to, time, transitionOffset);
        }

        bool playing() const { return _controller->playing(); }
        void setPlaying(const bool value) { _controller->setPlaying(value); }
        bool playable() const { return _controller->playable(); }

        const std::string& activeState() const { return _controller->activeStateName(); }
        float activeStateProgress() const { return _controller->activeStateProgress(); }
        float activeStateDuration() const { return _controller->activeStateDuration(); }
        bool transitioning() const { return _controller->transitioning(); }
        /** How far the current transition has run, 0 to 1 (1 when not transitioning). */
        float transitionProgress() const { return _controller->transitionProgress(); }
        /** The state a transition is blending away from; empty when there is none. */
        std::string previousState() const
        {
            const AnimState* state = _controller->previousState();
            return state ? state->name() : std::string();
        }

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
        size_t _index = 0;
        float _weight;
        AnimLayerBlendType _blendType = AnimLayerBlendType::OVERWRITE;
        std::unordered_set<std::string> _mask;
        std::vector<std::string> _maskSubtrees;
        uint64_t _maskVersion = 1;
        std::unique_ptr<AnimEvaluator> _evaluator;
        std::unique_ptr<AnimController> _controller;
    };
}
