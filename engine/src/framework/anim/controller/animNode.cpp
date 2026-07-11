// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "animNode.h"

#include "animBlendTree.h"
#include "animState.h"

namespace visutwin::canvas
{
    AnimNode::AnimNode(AnimState* state, AnimBlendTree* parent, std::string name,
                       const Vector2& point, const float speed)
        : _state(state), _parent(parent), _name(std::move(name)),
          _point(point), _pointLength(point.length()), _speed(speed)
    {
    }

    std::string AnimNode::path() const
    {
        return _parent ? _parent->path() + "." + _name : _name;
    }

    float AnimNode::weight() const
    {
        return _parent ? _parent->weight() * _weight : _weight;
    }

    float AnimNode::normalizedWeight() const
    {
        const float totalWeight = _state ? _state->totalWeight() : 0.0f;
        if (totalWeight == 0.0f) {
            return 0.0f;
        }
        return weight() / totalWeight;
    }
}
