// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.07.2026
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
    class GraphNode;
    class MorphInstance;

    /// What AnimComponent::update did with the animation.
    enum class AnimUpdateResult : uint8_t
    {
        EVALUATED = 0,  ///< evaluated and applied
        OFFSCREEN = 1,  ///< skipped: the component is not visible (offscreenUpdate)
        INTERVAL = 2    ///< skipped: not due under the evaluation interval
    };
    /**
     * The modern animation component: drives the entity hierarchy through a
     * state graph of layers, states, transitions with conditions, and blend trees, controlled
     * by named parameters. Complements the legacy AnimationComponent (single-clip cross-fade).
     */
    class AnimComponent : public Component
    {
    public:
        AnimComponent(IComponentSystem* system, Entity* entity);
        ~AnimComponent() override;

        void initializeComponentData() override {}
        void cloneFrom(const Component* source) override;


        /** Build layers and parameters from a state graph description. */
        void loadStateGraph(const AnimStateGraph& stateGraph);
        void removeStateGraph();

        AnimComponentLayer* baseLayer() const
        {
            return _layers.empty() ? nullptr : _layers.front().get();
        }
        AnimComponentLayer* findLayer(const std::string& name) const;
        /**
         * Adds a layer (or returns the one of that name): it starts in START with no
         * transition, so it drives nothing until a state is assigned an animation and
         * transitioned to. The mask lists the node paths it may drive, alone (`mask`) or with
         * everything beneath them (`maskSubtrees`); both empty, it drives every node.
         */
        AnimComponentLayer* addLayer(const std::string& name, float weight = 1.0f,
                                     std::unordered_set<std::string> mask = {},
                                     AnimLayerBlendType blendType = AnimLayerBlendType::OVERWRITE,
                                     std::vector<std::string> maskSubtrees = {});
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
         * Normalise layer weights by their sum per animated node. Off by default, a layer's weight is its plain
         * contribution: OVERWRITE blends toward the layer by that weight, ADDITIVE adds the
         * layer's offset from the rest pose scaled by it. On, the weights of the layers
         * driving a node are divided by their total and blended sequentially from
         * identity, and layers beneath the topmost OVERWRITE layer drop out.
         */
        bool normalizeWeights() const { return _normalizeWeights; }
        void setNormalizeWeights(const bool value) { _normalizeWeights = value; }

        /** A layer's evaluator reports its pose here (see AnimEvaluator::setPoseSink). */
        void accumulateLayerPose(size_t layerIndex, size_t slot, const std::string& nodePath,
            const AnimTransform& value);

        bool activate() const { return _activate; }
        void setActivate(const bool value) { _activate = value; }

        /**
         * How the component updates while it is not visible: no skinned or morphed mesh
         * instance on its root entity or beneath it was drawn in the previous frame, by any
         * camera or shadow pass. FULL (the default) always updates fully; STATE updates the
         * state graph, transitions and events but evaluates and applies nothing; NONE does not
         * update at all, resuming where it stopped. Visibility is the previous frame's, so a
         * character coming into view shows its last pose for one frame; an application that
         * predicts visibility (bounds against the camera frustum) sets FULL on characters about
         * to appear. A component animating no skinned or morphed mesh is never visible and
         * should keep FULL.
         */
        AnimOffscreenUpdate offscreenUpdate() const { return _offscreenUpdate; }
        void setOffscreenUpdate(const AnimOffscreenUpdate value) { _offscreenUpdate = value; }

        /**
         * Evaluate and apply the animation every Nth frame (1, the default, is every frame);
         * the state graph, transitions and events still update every frame. Components with
         * the same interval are spread over its frames, and a component that has just become
         * visible again is evaluated at once.
         */
        int evaluationInterval() const { return _evaluationInterval; }
        void setEvaluationInterval(int value);

        void reset();
        /// One update, with the visibility and the schedule read from this component's system
        /// and engine (without them the component counts as visible at update index 0).
        AnimUpdateResult update(float dt);
        /// One update against the render version of the frame just drawn (nullopt: visible)
        /// and the update index shared by the components with an evaluation interval.
        AnimUpdateResult update(float dt, std::optional<int> renderVersion, uint64_t updateIndex);

    private:
        void consumeTrigger(const std::string& name) { _consumedTriggers.insert(name); }


        std::vector<std::unique_ptr<AnimComponentLayer>> _layers;
        std::unordered_map<std::string, AnimParameter> _parameters;
        std::unordered_set<std::string> _consumedTriggers;

        float _speed = 1.0f;
        bool _activate = true;
        bool _normalizeWeights = false;
        AnimOffscreenUpdate _offscreenUpdate = AnimOffscreenUpdate::FULL;
        int _evaluationInterval = 1;
        // The frame of the interval this component evaluates in, spreading the components
        // sharing an interval over its frames.
        int _evaluationPhase = 0;
        // The animation was skipped as not visible, so it is evaluated as soon as it is seen.
        bool _evaluationOffscreen = false;

        // Per animated node: what each layer produced this update, and the node's
        // rest value per property, captured the first time a layer drives it. Kept across frames, INDEXED: a layer's
        // evaluator reports a node by its slot, which maps to a target once
        // (_layerSlotTargets). The binder's answers and each layer's mask test are cached
        // per target, so no node path is hashed per frame.
        struct LayerContribution
        {
            AnimTransform value;
            bool present = false;    // this update
        };
        struct TargetValue
        {
            std::string path;
            std::vector<LayerContribution> layers;    // by layer index
            AnimTransform base;
            uint64_t nodeVersion = UINT64_MAX;
            GraphNode* node = nullptr;
            uint64_t morphsVersion = UINT64_MAX;
            std::vector<MorphInstance*> morphs;
            std::vector<uint64_t> maskVersions;       // by layer; drives() answer below
            std::vector<uint8_t> drives;
        };
        std::vector<TargetValue> _targets;
        std::unordered_map<std::string, size_t> _targetLookup;      // a slot's first report only
        std::vector<std::vector<size_t>> _layerSlotTargets;          // [layer][slot] -> target
        std::vector<size_t> _touchedTargets;                         // this update
        std::vector<uint8_t> _targetTouched;
        std::vector<size_t> _activeLayers;                           // writeTarget scratch
        AnimTransform _composeScratch;                               // writeTarget's value
        void clearContributions();
        bool layerDrives(TargetValue& target, size_t layer);
        void writeComponentTarget(TargetValue& target, const std::string& nodePath, const std::string& component,
            const std::string& property);
        // The graph loadStateGraph was given, kept so a clone can load the same one.
        std::optional<AnimStateGraph> _stateGraph;
        std::unique_ptr<AnimBinder> _binder;   // resolves node paths for the final write

        void composeTargets();
        void writeTarget(TargetValue& target);

        friend class AnimComponentLayer;
    };
}
