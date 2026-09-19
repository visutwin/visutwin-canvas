// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include "framework/anim/evaluator/animTrack.h"
#include "framework/anim/binder/animBinder.h"
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "framework/components/component.h"
#include "framework/anim/state-graph/animStateGraph.h"
#include "animComponentLayer.h"

namespace visutwin::canvas
{
    /**
     * The modern animation component (upstream `anim`): drives the entity hierarchy through a
     * state graph of layers, states, transitions with conditions, and blend trees, controlled
     * by named parameters. Complements the legacy AnimationComponent (single-clip cross-fade).
     */
    class AnimComponent : public Component
    {
    public:
        AnimComponent(IComponentSystem* system, Entity* entity);
        ~AnimComponent() override;

        void initializeComponentData() override {}

        static const std::vector<AnimComponent*>& instances() { return _instances; }

        /** Build layers and parameters from a state graph description. */
        void loadStateGraph(const AnimStateGraph& stateGraph);
        void removeStateGraph();

        AnimComponentLayer* baseLayer() const
        {
            return _layers.empty() ? nullptr : _layers.front().get();
        }
        AnimComponentLayer* findAnimationLayer(const std::string& name) const;
        const std::vector<std::unique_ptr<AnimComponentLayer>>& layers() const { return _layers; }

        /**
         * Assign an animation track to a state (optionally a blend-tree leaf via
         * "State.Leaf" paths) on the given layer (base layer when empty).
         */
        void assignAnimation(const std::string& path, const std::shared_ptr<AnimTrack>& track,
                             const std::string& layerName = {},
                             std::optional<float> speed = std::nullopt,
                             std::optional<bool> loop = std::nullopt);

        // ── Parameters ────────────────────────────────────────────────
        float getFloat(const std::string& name) const;
        void setFloat(const std::string& name, float value);
        int getInteger(const std::string& name) const;
        void setInteger(const std::string& name, int value);
        bool getBoolean(const std::string& name) const;
        void setBoolean(const std::string& name, bool value);
        /** Set a trigger; it resets automatically once consumed by a transition. */
        void setTrigger(const std::string& name);
        void resetTrigger(const std::string& name);

        AnimParameter* findParameter(const std::string& name);

        // ── Playback ──────────────────────────────────────────────────
        bool playing() const;
        void setPlaying(bool value);

        float speed() const { return _speed; }
        void setSpeed(const float value) { _speed = value; }

        /**
         * Normalise layer weights by their sum per animated node (upstream
         * normalizeWeights). Off, upstream's default, a layer's weight is its plain
         * contribution: OVERWRITE blends toward the layer by that weight, ADDITIVE adds the
         * layer's offset from the rest pose scaled by it. On, the weights of the layers
         * driving a node are divided by their total and blended sequentially from
         * identity, and layers beneath the topmost OVERWRITE layer drop out, as upstream
         * masks them.
         */
        bool normalizeWeights() const { return _normalizeWeights; }
        void setNormalizeWeights(const bool value) { _normalizeWeights = value; }

        /** A layer's evaluator reports its pose here (see AnimEvaluator::setPoseSink). */
        void accumulateLayerPose(size_t layerIndex, const std::string& nodePath, const AnimTransform& value);

        bool activate() const { return _activate; }
        void setActivate(const bool value) { _activate = value; }

        void reset();
        void update(float dt);

    private:
        void consumeTrigger(const std::string& name) { _consumedTriggers.insert(name); }

        inline static std::vector<AnimComponent*> _instances;

        std::vector<std::unique_ptr<AnimComponentLayer>> _layers;
        std::unordered_map<std::string, AnimParameter> _parameters;
        std::unordered_set<std::string> _consumedTriggers;

        float _speed = 1.0f;
        bool _activate = true;
        bool _normalizeWeights = false;

        // Per animated node: what each layer produced this update, and the node's
        // rest value per property, captured the first time a layer drives it — the
        // baseValue upstream reads at bind. Kept across frames; contributions are
        // cleared every update.
        struct LayerContribution
        {
            size_t layer = 0;
            AnimTransform value;
        };
        struct TargetValue
        {
            std::vector<LayerContribution> contributions;
            AnimTransform base;
        };
        std::unordered_map<std::string, TargetValue> _targets;
        std::unique_ptr<AnimBinder> _binder;   // resolves node paths for the final write

        void composeTargets();
        void writeTarget(const std::string& nodePath, TargetValue& target);

        friend class AnimComponentLayer;
    };
}
