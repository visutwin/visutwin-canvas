// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <cmath>
#include <memory>
#include <string>

#include <core/math/vector2.h>

#include "framework/anim/evaluator/animTrack.h"

namespace visutwin::canvas
{
    class AnimBlendTree;
    class AnimState;

    /**
     * AnimNodes represent a single animation track in the current state. Each state can contain
     * multiple AnimNodes, in which case they are stored in an AnimBlendTree hierarchy, which
     * controls the weight (contribution to the state's final animation) of its children.
     */
    class AnimNode
    {
    public:
        AnimNode(AnimState* state, AnimBlendTree* parent, std::string name, const Vector2& point,
                 float speed = 1.0f);
        virtual ~AnimNode() = default;

        AnimBlendTree* parent() const { return _parent; }
        const std::string& name() const { return _name; }
        std::string path() const;

        const Vector2& point() const { return _point; }
        float pointLength() const { return _pointLength; }

        void setWeight(const float value) { _weight = value; }
        float rawWeight() const { return _weight; }

        /** Effective weight: this node's weight scaled by its parent chain. */
        virtual float weight() const;

        /** Weight normalized against the owning state's total animation weight. */
        float normalizedWeight() const;

        float speed() const { return _weightedSpeed * _speed; }
        float absoluteSpeed() const { return std::abs(_speed); }
        void setWeightedSpeed(const float value) { _weightedSpeed = value; }

        const std::shared_ptr<AnimTrack>& animTrack() const { return _animTrack; }
        void setAnimTrack(const std::shared_ptr<AnimTrack>& track) { _animTrack = track; }

        virtual bool isBlendTree() const { return false; }
        virtual bool syncAnimations() const { return false; }

    protected:
        AnimState* _state;
        AnimBlendTree* _parent;
        std::string _name;
        Vector2 _point;
        float _pointLength;
        float _speed;
        float _weightedSpeed = 1.0f;
        float _weight = 1.0f;
        std::shared_ptr<AnimTrack> _animTrack;
    };
}
