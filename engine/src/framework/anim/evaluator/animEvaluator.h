// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "framework/anim/binder/animBinder.h"
#include <functional>
#include <string>

#include "animClip.h"

namespace visutwin::canvas
{
    class AnimEvaluator
    {
    public:
        explicit AnimEvaluator(std::unique_ptr<AnimBinder> binder);

        const std::vector<std::shared_ptr<AnimClip>>& clips() const { return _clips; }
        std::vector<std::shared_ptr<AnimClip>>& clips() { return _clips; }

        void addClip(const std::shared_ptr<AnimClip>& clip);
        void removeClip(size_t index);
        void removeClips();

        /** Find a clip by name (nullptr when absent). */
        AnimClip* findClip(const std::string& name) const;

        /** Replace the track of every clip whose name matches (used by AnimController::assignAnimation). */
        void updateClipTrack(const std::string& name, const std::shared_ptr<AnimTrack>& track);

        void update(float dt);

        /**
         * Where the blended pose goes. By default the evaluator writes each node's
         * transform (and morph weights) straight through its binder. With a sink set
         * it hands the per-node result to the sink instead and touches nothing: this is
         * how an AnimComponent collects every layer's pose and composes them by layer
         * weight before a single write (upstream AnimTargetValue). The sink receives one
         * call per animated node per update, keyed by the curve's node path.
         */
        using PoseSink = std::function<void(const std::string& nodePath, const AnimTransform& value)>;
        void setPoseSink(PoseSink sink) { _poseSink = std::move(sink); }
        AnimBinder* binder() const { return _binder.get(); }

    private:
        std::unique_ptr<AnimBinder> _binder;
        PoseSink _poseSink;
        std::vector<std::shared_ptr<AnimClip>> _clips;
    };
}
