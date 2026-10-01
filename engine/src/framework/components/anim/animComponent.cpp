// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "animComponent.h"
#include "framework/entity.h"
#include "scene/graphNode.h"
#include "scene/morphInstance.h"
#include "framework/anim/evaluator/animEvaluator.h"
#include "framework/anim/binder/defaultAnimBinder.h"
#include "framework/anim/controller/animNode.h"
#include "framework/anim/controller/animState.h"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    AnimComponent::AnimComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instanceList.add(this);
    }

    AnimComponent::~AnimComponent()
    {
        _instanceList.remove(this);
    }

    void AnimComponent::loadStateGraph(const AnimStateGraph& stateGraph)
    {
        removeStateGraph();
        _stateGraph = stateGraph;
        _parameters = stateGraph.parameters();
        _binder = std::make_unique<DefaultAnimBinder>(entity());
        for (const auto& layerDesc : stateGraph.layers()) {
            _layers.push_back(std::make_unique<AnimComponentLayer>(
                layerDesc.name, _layers.size(), this, layerDesc.states, layerDesc.transitions,
                layerDesc.weight, layerDesc.blendType, layerDesc.mask, _activate));
        }
        if (_layers.empty()) {
            spdlog::warn("AnimComponent::loadStateGraph: state graph has no layers");
        }
    }

    void AnimComponent::removeStateGraph()
    {
        _layers.clear();
        _stateGraph.reset();
        _targets.clear();
        _targetLookup.clear();
        _layerSlotTargets.clear();
        _touchedTargets.clear();
        _targetTouched.clear();
        _binder.reset();
        _parameters.clear();
        _consumedTriggers.clear();
    }

    void AnimComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const AnimComponent*>(source);
        if (!src) {
            return;
        }
        // Upstream's cloneComponent: the settings, the same state graph, every layer's
        // weight, blend type and mask, the animations assigned to each state, the
        // parameter values and the playing flag. The binder is built on THIS entity,
        // so the clone animates its own subtree by the same node paths.
        _speed = src->_speed;
        _activate = src->_activate;
        _normalizeWeights = src->_normalizeWeights;
        if (!src->_stateGraph) {
            return;
        }
        loadStateGraph(*src->_stateGraph);
        _parameters = src->_parameters;
        for (size_t i = 0; i < _layers.size() && i < src->_layers.size(); ++i) {
            AnimComponentLayer& layer = *_layers[i];
            const AnimComponentLayer& srcLayer = *src->_layers[i];
            layer.setWeight(srcLayer.weight());
            layer.setBlendType(srcLayer.blendType());
            layer.setMask(srcLayer.mask());
            const AnimController* controller = srcLayer.controller();
            for (const auto& stateName : controller->states()) {
                const AnimState* state = controller->state(stateName);
                if (!state) {
                    continue;
                }
                for (const AnimNode* node : state->animations()) {
                    if (node && node->animTrack()) {
                        layer.assignAnimation(node->path(), node->animTrack(), state->speed(), state->loop());
                    }
                }
            }
        }
        setPlaying(src->playing());
    }

    AnimComponentLayer* AnimComponent::findAnimationLayer(const std::string& name) const
    {
        for (const auto& layer : _layers) {
            if (layer->name() == name) {
                return layer.get();
            }
        }
        return nullptr;
    }

    void AnimComponent::assignAnimation(const std::string& path, const std::shared_ptr<AnimTrack>& track,
        const std::string& layerName, const std::optional<float> speed, const std::optional<bool> loop)
    {
        // With no state graph, a plain state name makes one: a Base layer that plays that state
        // from the start (upstream's default graph).
        if (_layers.empty() && path.find('.') == std::string::npos) {
            AnimStateGraph stateGraph;
            auto& layer = stateGraph.addLayer("Base");
            layer.states.push_back(AnimStateDesc{path, speed.value_or(1.0f), loop.value_or(true)});
            layer.transitions.push_back(AnimTransitionDesc{.from = ANIM_STATE_START, .to = path});
            loadStateGraph(stateGraph);
            baseLayer()->assignAnimation(path, track);
            return;
        }
        AnimComponentLayer* layer = layerName.empty() ? baseLayer() : findAnimationLayer(layerName);
        if (!layer) {
            spdlog::error("AnimComponent::assignAnimation: no layer '{}'", layerName.empty() ? "<base>" : layerName);
            return;
        }
        layer->assignAnimation(path, track, speed, loop);
    }

    AnimParameter* AnimComponent::findParameter(const std::string& name)
    {
        const auto it = _parameters.find(name);
        return it != _parameters.end() ? &it->second : nullptr;
    }

    float AnimComponent::getFloat(const std::string& name) const
    {
        const auto it = _parameters.find(name);
        return it != _parameters.end() ? it->second.value : 0.0f;
    }

    void AnimComponent::setFloat(const std::string& name, const float value)
    {
        if (AnimParameter* parameter = findParameter(name);
            parameter && parameter->type == AnimParameterType::FLOAT) {
            parameter->value = value;
        } else {
            spdlog::warn("AnimComponent::setFloat: no float parameter '{}'", name);
        }
    }

    int AnimComponent::getInteger(const std::string& name) const
    {
        const auto it = _parameters.find(name);
        return it != _parameters.end() ? static_cast<int>(it->second.value) : 0;
    }

    void AnimComponent::setInteger(const std::string& name, const int value)
    {
        if (AnimParameter* parameter = findParameter(name);
            parameter && parameter->type == AnimParameterType::INTEGER) {
            parameter->value = static_cast<float>(value);
        } else {
            spdlog::warn("AnimComponent::setInteger: no integer parameter '{}'", name);
        }
    }

    bool AnimComponent::getBoolean(const std::string& name) const
    {
        const auto it = _parameters.find(name);
        return it != _parameters.end() && it->second.value != 0.0f;
    }

    void AnimComponent::setBoolean(const std::string& name, const bool value)
    {
        if (AnimParameter* parameter = findParameter(name);
            parameter && parameter->type == AnimParameterType::BOOLEAN) {
            parameter->value = value ? 1.0f : 0.0f;
        } else {
            spdlog::warn("AnimComponent::setBoolean: no boolean parameter '{}'", name);
        }
    }

    void AnimComponent::setTrigger(const std::string& name)
    {
        if (AnimParameter* parameter = findParameter(name);
            parameter && parameter->type == AnimParameterType::TRIGGER) {
            parameter->value = 1.0f;
        } else {
            spdlog::warn("AnimComponent::setTrigger: no trigger parameter '{}'", name);
        }
    }

    void AnimComponent::resetTrigger(const std::string& name)
    {
        if (AnimParameter* parameter = findParameter(name);
            parameter && parameter->type == AnimParameterType::TRIGGER) {
            parameter->value = 0.0f;
        }
    }

    bool AnimComponent::playing() const
    {
        for (const auto& layer : _layers) {
            if (layer->playing()) {
                return true;
            }
        }
        return false;
    }

    void AnimComponent::setPlaying(const bool value)
    {
        for (const auto& layer : _layers) {
            layer->setPlaying(value);
        }
    }

    void AnimComponent::reset()
    {
        for (const auto& layer : _layers) {
            layer->reset();
        }
        _consumedTriggers.clear();
    }

    void AnimComponent::accumulateLayerPose(const size_t layerIndex, const size_t slot,
        const std::string& nodePath, const AnimTransform& value)
    {
        if (_layerSlotTargets.size() <= layerIndex) {
            _layerSlotTargets.resize(layerIndex + 1);
        }
        auto& slotTargets = _layerSlotTargets[layerIndex];
        if (slotTargets.size() <= slot) {
            slotTargets.resize(slot + 1, SIZE_MAX);
        }
        size_t targetIndex = slotTargets[slot];
        if (targetIndex == SIZE_MAX) {
            const auto [it, inserted] = _targetLookup.try_emplace(nodePath, _targets.size());
            if (inserted) {
                _targets.emplace_back();
                _targets.back().path = nodePath;
                _targetTouched.push_back(0);
            }
            targetIndex = it->second;
            slotTargets[slot] = targetIndex;
        }

        TargetValue& target = _targets[targetIndex];
        if (target.layers.size() <= layerIndex) {
            target.layers.resize(layerIndex + 1);
        }
        // Assigned, not appended: the weights vector keeps its storage frame to frame.
        target.layers[layerIndex].value = value;
        target.layers[layerIndex].present = true;
        if (!_targetTouched[targetIndex]) {
            _targetTouched[targetIndex] = 1;
            _touchedTargets.push_back(targetIndex);
        }
    }

    void AnimComponent::clearContributions()
    {
        for (const size_t targetIndex : _touchedTargets) {
            for (auto& contribution : _targets[targetIndex].layers) {
                contribution.present = false;
            }
            _targetTouched[targetIndex] = 0;
        }
        _touchedTargets.clear();
    }

    bool AnimComponent::layerDrives(TargetValue& target, const size_t layer)
    {
        const AnimComponentLayer& animLayer = *_layers[layer];
        if (animLayer.mask().empty()) {
            return true;
        }
        if (target.maskVersions.size() <= layer) {
            target.maskVersions.resize(layer + 1, 0);
            target.drives.resize(layer + 1, 0);
        }
        if (target.maskVersions[layer] != animLayer.maskVersion()) {
            target.drives[layer] = animLayer.drives(target.path) ? 1 : 0;
            target.maskVersions[layer] = animLayer.maskVersion();
        }
        return target.drives[layer] != 0;
    }

    void AnimComponent::update(const float dt)
    {
        clearContributions();
        // Every layer advances, whatever its weight (upstream updates them all); a
        // weight of 0 contributes nothing in composeTargets but keeps the layer's time
        // moving, so fading it back in resumes where it would have been.
        for (const auto& layer : _layers) {
            layer->update(dt * _speed);
        }
        composeTargets();
        // Reset triggers consumed by transitions this frame (upstream consumes at frame end).
        for (const auto& name : _consumedTriggers) {
            resetTrigger(name);
        }
        _consumedTriggers.clear();
    }

    namespace
    {
        Quaternion identityQuat() { return Quaternion(0.0f, 0.0f, 0.0f, 1.0f); }
    }

    void AnimComponent::composeTargets()
    {
        if (!_binder) {
            return;
        }
        for (const size_t targetIndex : _touchedTargets) {
            writeTarget(_targets[targetIndex]);
        }
    }

    // Upstream AnimTargetValue.updateValue, run once per node per update over the
    // layers that drove it, in layer order. Off normalisation the value starts at the
    // node's rest pose; on it, at identity, and only the topmost OVERWRITE layer and
    // the layers above it take part (upstream zeroes the masks beneath it).
    void AnimComponent::writeTarget(TargetValue& target)
    {
        if (target.nodeVersion != _binder->version()) {
            target.node = _binder->resolve(target.path);
            target.nodeVersion = _binder->version();
        }
        GraphNode* node = target.node;
        bool anyWeights = false;
        for (const auto& c : target.layers) {
            anyWeights = anyWeights || (c.present && c.value.hasWeights);
        }
        static const std::vector<MorphInstance*> noMorphs;
        const std::vector<MorphInstance*>* morphsFound = &noMorphs;
        if (anyWeights) {
            if (target.morphsVersion != _binder->version()) {
                target.morphs = _binder->resolveMorphInstances(target.path);
                target.morphsVersion = _binder->version();
            }
            morphsFound = &target.morphs;
        }
        const std::vector<MorphInstance*>& morphs = *morphsFound;
        if (!node && morphs.empty()) {
            return;
        }

        // Layers that contributed and may drive this node, in layer order.
        std::vector<size_t>& active = _activeLayers;
        active.clear();
        for (size_t layer = 0; layer < target.layers.size() && layer < _layers.size(); ++layer) {
            if (target.layers[layer].present && layerDrives(target, layer)) {
                active.push_back(layer);
            }
        }
        if (active.empty()) {
            return;
        }
        if (_normalizeWeights) {
            size_t firstKept = 0;
            for (size_t i = 0; i < active.size(); ++i) {
                if (_layers[active[i]]->blendType() == AnimLayerBlendType::OVERWRITE) {
                    firstKept = i;
                }
            }
            active.erase(active.begin(), active.begin() + static_cast<std::ptrdiff_t>(firstKept));
        }
        float total = 0.0f;
        for (const size_t layer : active) {
            total += _layers[layer]->weight();
        }
        const auto weightOf = [&](const size_t layer) {
            const float w = _layers[layer]->weight();
            if (_normalizeWeights) {
                return total > 0.0f ? w / total : 0.0f;
            }
            return std::clamp(w, 0.0f, 1.0f);
        };

        // Rest pose, captured once per property the first time a layer drives it.
        AnimTransform& base = target.base;
        if (node) {
            if (!base.hasPosition) { base.position = node->localPosition(); base.hasPosition = true; }
            if (!base.hasRotation) { base.rotation = node->localRotation(); base.hasRotation = true; }
            if (!base.hasScale) { base.scale = node->localScale(); base.hasScale = true; }
        }
        if (anyWeights && !base.hasWeights) {
            for (const size_t layer : active) {
                const AnimTransform& v = target.layers[layer].value;
                if (v.hasWeights) { base.weights.assign(v.weights.size(), 0.0f); break; }
            }
            if (!morphs.empty() && morphs.front()) {
                for (size_t i = 0; i < base.weights.size(); ++i) {
                    if (static_cast<int>(i) < morphs.front()->weightCount()) {
                        base.weights[i] = morphs.front()->weight(static_cast<int>(i));
                    }
                }
            }
            base.hasWeights = true;
        }

        // Start value: rest pose, or identity when normalising (upstream). Kept across
        // calls so its weights vector keeps its storage.
        AnimTransform& value = _composeScratch;
        if (_normalizeWeights) {
            value.position = Vector3(0.0f);
            value.rotation = identityQuat();
            value.scale = Vector3(0.0f);
            value.weights.assign(base.weights.size(), 0.0f);
        } else {
            value = base;
        }
        value.hasPosition = value.hasRotation = value.hasScale = value.hasWeights = false;

        for (const size_t layer : active) {
            const float w = weightOf(layer);
            if (w <= 0.0f) {
                continue;
            }
            const bool additive = !_normalizeWeights &&
                _layers[layer]->blendType() == AnimLayerBlendType::ADDITIVE;
            const AnimTransform& v = target.layers[layer].value;
            if (v.hasPosition) {
                value.position = additive
                    ? value.position + (v.position - base.position) * w
                    : Vector3::lerp(value.position, v.position, w);
                value.hasPosition = true;
            }
            if (v.hasRotation) {
                if (additive) {
                    // Offset from the rest rotation, scaled toward identity, applied on top.
                    const Quaternion offset = base.rotation.conjugate() * v.rotation;
                    value.rotation = value.rotation * Quaternion::slerp(identityQuat(), offset, w);
                } else {
                    value.rotation = Quaternion::nlerp(value.rotation, v.rotation, w);
                }
                value.hasRotation = true;
            }
            if (v.hasScale) {
                value.scale = additive
                    ? value.scale + (v.scale - base.scale) * w
                    : Vector3::lerp(value.scale, v.scale, w);
                value.hasScale = true;
            }
            if (v.hasWeights) {
                if (value.weights.size() < v.weights.size()) value.weights.resize(v.weights.size(), 0.0f);
                for (size_t i = 0; i < v.weights.size(); ++i) {
                    const float b = i < base.weights.size() ? base.weights[i] : 0.0f;
                    value.weights[i] = additive
                        ? value.weights[i] + (v.weights[i] - b) * w
                        : value.weights[i] + (v.weights[i] - value.weights[i]) * w;
                }
                value.hasWeights = true;
            }
        }

        if (node) {
            if (value.hasPosition) node->setLocalPosition(value.position);
            if (value.hasRotation) node->setLocalRotation(value.rotation);
            if (value.hasScale) node->setLocalScale(value.scale);
        }
        if (value.hasWeights) {
            for (auto* morph : morphs) {
                if (!morph) continue;
                for (size_t i = 0; i < value.weights.size(); ++i) {
                    morph->setWeight(static_cast<int>(i), value.weights[i]);
                }
            }
        }
    }
}
