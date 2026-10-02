// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "framework/anim/binder/animBinder.h"
#include <functional>
#include <string>

#include "animClip.h"

namespace visutwin::canvas
{
    class GraphNode;
    class MorphInstance;

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
         * weight before a single write. The sink receives one
         * call per animated node per update, with the curve's node path and the node's
         * SLOT: a small integer this evaluator gives each node path it has seen, stable
         * for the evaluator's lifetime, so the sink can index rather than hash.
         * Only the value's flagged fields (hasPosition, ...) carry this update's result.
         */
        using PoseSink = std::function<void(size_t slot, const std::string& nodePath, const AnimTransform& value)>;
        void setPoseSink(PoseSink sink) { _poseSink = std::move(sink); }
        AnimBinder* binder() const { return _binder.get(); }

        /// Node paths this evaluator has given a slot, indexed by slot.
        const std::vector<std::string>& slotPaths() const { return _slotPaths; }

    private:
        // One per node path the clips have driven, kept across updates. The blend
        // accumulates here (counters reset on the first contribution of each update),
        // and the binder's answers for the path are cached against binder->version().
        struct Slot
        {
            AnimTransform value;
            int posCounter = 0;
            int rotCounter = 0;
            int sclCounter = 0;
            int wgtCounter = 0;
            bool touched = false;
            uint64_t resolvedVersion = UINT64_MAX;
            GraphNode* node = nullptr;
            uint64_t morphsVersion = UINT64_MAX;
            std::vector<MorphInstance*> morphs;
        };
        // How one track's targets map onto slots, keyed by AnimTrack::serial().
        struct TrackBinding
        {
            std::vector<size_t> slots;
        };

        const TrackBinding& bindingFor(const AnimTrack& track);
        GraphNode* nodeFor(Slot& slot, const std::string& path);
        const std::vector<MorphInstance*>& morphsFor(Slot& slot, const std::string& path);

        std::unique_ptr<AnimBinder> _binder;
        PoseSink _poseSink;
        std::vector<std::shared_ptr<AnimClip>> _clips;

        std::vector<Slot> _slots;
        std::vector<std::string> _slotPaths;
        std::unordered_map<std::string, size_t> _slotLookup;          // new targets only
        std::unordered_map<uint64_t, TrackBinding> _trackBindings;
        std::vector<size_t> _touchedSlots;                             // this update, in first-touched order
        std::vector<AnimTransform> _scratch;                           // one clip's evaluation
        std::vector<uint8_t> _scratchTouched;
    };
}
