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

#include <algorithm>

#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    AnimComponent::AnimComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    AnimComponent::~AnimComponent()
    {
        const auto it = std::find(_instances.begin(), _instances.end(), this);
        if (it != _instances.end()) {
            _instances.erase(it);
        }
    }

    void AnimComponent::loadStateGraph(const AnimStateGraph& stateGraph)
    {
        removeStateGraph();
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
        _targets.clear();
        _binder.reset();
        _parameters.clear();
        _consumedTriggers.clear();
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
        AnimComponentLayer* layer = layerName.empty() ? baseLayer() : findAnimationLayer(layerName);
        if (!layer) {
            spdlog::error("AnimComponent::assignAnimation: no layer '{}' — call loadStateGraph first",
                layerName.empty() ? "<base>" : layerName);
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

    void AnimComponent::accumulateLayerPose(const size_t layerIndex, const std::string& nodePath,
        const AnimTransform& value)
    {
        _targets[nodePath].contributions.push_back(LayerContribution{layerIndex, value});
    }

    void AnimComponent::update(const float dt)
    {
        for (auto& [path, target] : _targets) {
            target.contributions.clear();
        }
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
        for (auto& [path, target] : _targets) {
            if (!target.contributions.empty()) {
                writeTarget(path, target);
            }
        }
    }

    // Upstream AnimTargetValue.updateValue, run once per node per update over the
    // layers that drove it, in layer order. Off normalisation the value starts at the
    // node's rest pose; on it, at identity, and only the topmost OVERWRITE layer and
    // the layers above it take part (upstream zeroes the masks beneath it).
    void AnimComponent::writeTarget(const std::string& nodePath, TargetValue& target)
    {
        GraphNode* node = _binder->resolve(nodePath);
        std::vector<MorphInstance*> morphs;
        bool anyWeights = false;
        for (const auto& c : target.contributions) {
            anyWeights = anyWeights || c.value.hasWeights;
        }
        if (anyWeights) {
            morphs = _binder->resolveMorphInstances(nodePath);
        }
        if (!node && morphs.empty()) {
            return;
        }

        // Contributions from layers that may drive this node, in layer order.
        std::vector<const LayerContribution*> active;
        for (const auto& c : target.contributions) {
            if (c.layer < _layers.size() && _layers[c.layer]->drives(nodePath)) {
                active.push_back(&c);
            }
        }
        if (active.empty()) {
            return;
        }
        if (_normalizeWeights) {
            size_t firstKept = 0;
            for (size_t i = 0; i < active.size(); ++i) {
                if (_layers[active[i]->layer]->blendType() == AnimLayerBlendType::OVERWRITE) {
                    firstKept = i;
                }
            }
            active.erase(active.begin(), active.begin() + static_cast<std::ptrdiff_t>(firstKept));
        }
        float total = 0.0f;
        for (const auto* c : active) {
            total += _layers[c->layer]->weight();
        }
        const auto weightOf = [&](const LayerContribution& c) {
            const float w = _layers[c.layer]->weight();
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
            for (const auto* c : active) {
                if (c->value.hasWeights) { base.weights.assign(c->value.weights.size(), 0.0f); break; }
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

        // Start value: rest pose, or identity when normalising (upstream).
        AnimTransform value;
        if (_normalizeWeights) {
            value.position = Vector3(0.0f);
            value.rotation = identityQuat();
            value.scale = Vector3(0.0f);
            value.weights.assign(base.weights.size(), 0.0f);
        } else {
            value = base;
        }
        value.hasPosition = value.hasRotation = value.hasScale = value.hasWeights = false;

        for (const auto* c : active) {
            const float w = weightOf(*c);
            if (w <= 0.0f) {
                continue;
            }
            const bool additive = !_normalizeWeights &&
                _layers[c->layer]->blendType() == AnimLayerBlendType::ADDITIVE;
            const AnimTransform& v = c->value;
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
