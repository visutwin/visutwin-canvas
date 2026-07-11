// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "animComponent.h"

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
        for (const auto& layerDesc : stateGraph.layers()) {
            _layers.push_back(std::make_unique<AnimComponentLayer>(
                layerDesc.name, this, layerDesc.states, layerDesc.transitions,
                layerDesc.weight, _activate));
        }
        if (_layers.empty()) {
            spdlog::warn("AnimComponent::loadStateGraph: state graph has no layers");
        }
    }

    void AnimComponent::removeStateGraph()
    {
        _layers.clear();
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

    void AnimComponent::update(const float dt)
    {
        for (const auto& layer : _layers) {
            if (layer->weight() > 0.0f) {
                layer->update(dt * _speed);
            }
        }
        // Reset triggers consumed by transitions this frame (upstream consumes at frame end).
        for (const auto& name : _consumedTriggers) {
            resetTrigger(name);
        }
        _consumedTriggers.clear();
    }
}
