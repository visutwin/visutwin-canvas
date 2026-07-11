// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "framework/anim/controller/animController.h"

namespace visutwin::canvas
{
    /** One animation layer: an independent state machine with its own states and transitions. */
    struct AnimLayerDesc
    {
        std::string name;
        std::vector<AnimStateDesc> states;
        std::vector<AnimTransitionDesc> transitions;
        float weight = 1.0f;
    };

    /**
     * The data model describing an anim component's state graph: layers of states and
     * transitions plus the parameters that drive them.
     *
     * DEVIATION: built through a typed builder API instead of upstream's untyped JSON
     * object (AnimStateGraph data with layers/parameters keys).
     */
    class AnimStateGraph
    {
    public:
        /** Add a layer and return it for population. The first layer added is the base layer. */
        AnimLayerDesc& addLayer(const std::string& name, const float weight = 1.0f)
        {
            _layers.push_back(AnimLayerDesc{name, {}, {}, weight});
            return _layers.back();
        }

        void addParameter(const std::string& name, const AnimParameterType type, const float value = 0.0f)
        {
            _parameters[name] = AnimParameter{type, value};
        }

        void addFloatParameter(const std::string& name, const float value = 0.0f)
        {
            addParameter(name, AnimParameterType::FLOAT, value);
        }

        void addIntegerParameter(const std::string& name, const int value = 0)
        {
            addParameter(name, AnimParameterType::INTEGER, static_cast<float>(value));
        }

        void addBooleanParameter(const std::string& name, const bool value = false)
        {
            addParameter(name, AnimParameterType::BOOLEAN, value ? 1.0f : 0.0f);
        }

        void addTriggerParameter(const std::string& name)
        {
            addParameter(name, AnimParameterType::TRIGGER, 0.0f);
        }

        const std::vector<AnimLayerDesc>& layers() const { return _layers; }
        const std::unordered_map<std::string, AnimParameter>& parameters() const { return _parameters; }

    private:
        std::vector<AnimLayerDesc> _layers;
        std::unordered_map<std::string, AnimParameter> _parameters;
    };
}
