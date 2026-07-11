// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "animBlendTree.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    namespace
    {
        // Signed angle between two vectors (upstream Vec2.angleRad).
        float angleRad(const Vector2& a, const Vector2& b)
        {
            return std::atan2(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
        }

        bool between(const float value, const float min, const float max)
        {
            return value >= min && value <= max;
        }

        uint64_t pointKey(const size_t i, const size_t j)
        {
            return (static_cast<uint64_t>(i) << 32) | static_cast<uint64_t>(j);
        }
    }

    AnimBlendTree::AnimBlendTree(AnimState* state, AnimBlendTree* parent, const std::string& name,
        const Vector2& point, std::vector<std::string> parameters,
        const std::vector<AnimBlendTreeDesc>& children, const bool syncAnimations,
        const AnimFindParameterFn& findParameter)
        : AnimNode(state, parent, name, point),
          _parameters(std::move(parameters)),
          _findParameter(findParameter),
          _syncAnimations(syncAnimations)
    {
        // Seed with NaN so the first calculateWeights() always runs.
        _parameterValues.resize(_parameters.size(), std::numeric_limits<float>::quiet_NaN());
        for (const auto& childDesc : children) {
            _children.push_back(create(childDesc, state, this, findParameter));
        }
    }

    std::unique_ptr<AnimNode> AnimBlendTree::create(const AnimBlendTreeDesc& desc, AnimState* state,
        AnimBlendTree* parent, const AnimFindParameterFn& findParameter)
    {
        if (desc.children.empty()) {
            return std::make_unique<AnimNode>(state, parent, desc.name, desc.point, desc.speed);
        }
        switch (desc.type) {
            case AnimBlendType::BLEND_1D: {
                // 1D trees sort their children by point.x at build time.
                auto children = desc.children;
                std::sort(children.begin(), children.end(),
                    [](const AnimBlendTreeDesc& a, const AnimBlendTreeDesc& b) {
                        return a.point.x < b.point.x;
                    });
                return std::make_unique<AnimBlendTree1D>(state, parent, desc.name, desc.point,
                    desc.parameters, children, desc.syncAnimations, findParameter);
            }
            case AnimBlendType::BLEND_2D_CARTESIAN:
                return std::make_unique<AnimBlendTreeCartesian2D>(state, parent, desc.name, desc.point,
                    desc.parameters, desc.children, desc.syncAnimations, findParameter);
            case AnimBlendType::BLEND_2D_DIRECTIONAL:
                return std::make_unique<AnimBlendTreeDirectional2D>(state, parent, desc.name, desc.point,
                    desc.parameters, desc.children, desc.syncAnimations, findParameter);
            case AnimBlendType::BLEND_DIRECT:
                return std::make_unique<AnimBlendTreeDirect>(state, parent, desc.name, desc.point,
                    desc.parameters, desc.children, desc.syncAnimations, findParameter);
        }
        spdlog::error("Invalid anim blend type: {}", static_cast<int>(desc.type));
        return nullptr;
    }

    float AnimBlendTree::weight() const
    {
        const_cast<AnimBlendTree*>(this)->calculateWeights();
        return _parent ? _parent->weight() * _weight : _weight;
    }

    AnimNode* AnimBlendTree::getChild(const std::string& name) const
    {
        for (const auto& child : _children) {
            if (child && child->name() == name) {
                return child.get();
            }
        }
        return nullptr;
    }

    int AnimBlendTree::getNodeCount() const
    {
        int count = 0;
        for (const auto& child : _children) {
            if (child && child->isBlendTree()) {
                count += static_cast<const AnimBlendTree*>(child.get())->getNodeCount();
            } else if (child) {
                count++;
            }
        }
        return count;
    }

    bool AnimBlendTree::updateParameterValues()
    {
        bool paramsEqual = true;
        for (size_t i = 0; i < _parameterValues.size(); ++i) {
            const AnimParameter* parameter = _findParameter ? _findParameter(_parameters[i]) : nullptr;
            const float updated = parameter ? parameter->value : 0.0f;
            if (_parameterValues[i] != updated) {
                _parameterValues[i] = updated;
                paramsEqual = false;
            }
        }
        return paramsEqual;
    }

    // ── 1D ──────────────────────────────────────────────────────────────

    void AnimBlendTree1D::calculateWeights()
    {
        if (updateParameterValues() || _children.empty()) {
            return;
        }
        float weightedDurationSum = 0.0f;
        _children[0]->setWeight(0.0f);
        for (size_t i = 0; i < _children.size(); ++i) {
            AnimNode* c1 = _children[i].get();
            if (i != _children.size() - 1) {
                AnimNode* c2 = _children[i + 1].get();
                if (c1->point().x == c2->point().x) {
                    c1->setWeight(0.5f);
                    c2->setWeight(0.5f);
                } else if (between(_parameterValues[0], c1->point().x, c2->point().x)) {
                    const float childDistance = std::abs(c1->point().x - c2->point().x);
                    const float parameterDistance = std::abs(c1->point().x - _parameterValues[0]);
                    const float weight = (childDistance - parameterDistance) / childDistance;
                    c1->setWeight(weight);
                    c2->setWeight(1.0f - weight);
                } else {
                    c2->setWeight(0.0f);
                }
            }
            if (_syncAnimations && c1->animTrack()) {
                weightedDurationSum += c1->animTrack()->duration() / c1->absoluteSpeed() * c1->rawWeight();
            }
        }
        if (_syncAnimations && weightedDurationSum > 0.0f) {
            for (const auto& child : _children) {
                if (child->animTrack()) {
                    child->setWeightedSpeed(
                        child->animTrack()->duration() / child->absoluteSpeed() / weightedDurationSum);
                }
            }
        }
    }

    // ── 2D cartesian ────────────────────────────────────────────────────

    const Vector2& AnimBlendTreeCartesian2D::pointDistanceCache(const size_t i, const size_t j) const
    {
        const auto key = pointKey(i, j);
        auto it = _pointCache.find(key);
        if (it == _pointCache.end()) {
            it = _pointCache.emplace(key, _children[j]->point() - _children[i]->point()).first;
        }
        return it->second;
    }

    void AnimBlendTreeCartesian2D::calculateWeights()
    {
        if (updateParameterValues() || _children.empty()) {
            return;
        }
        const Vector2 p(_parameterValues.size() > 0 ? _parameterValues[0] : 0.0f,
                        _parameterValues.size() > 1 ? _parameterValues[1] : 0.0f);
        float weightSum = 0.0f;
        float weightedDurationSum = 0.0f;
        for (size_t i = 0; i < _children.size(); ++i) {
            AnimNode* child = _children[i].get();
            const Vector2 pip = p - child->point();
            float minj = std::numeric_limits<float>::max();
            for (size_t j = 0; j < _children.size(); ++j) {
                if (i == j) continue;
                const Vector2& pipj = pointDistanceCache(i, j);
                const float lengthSq = pipj.dot(pipj);
                const float result = lengthSq > 0.0f
                    ? std::clamp(1.0f - (pip.dot(pipj) / lengthSq), 0.0f, 1.0f) : 0.0f;
                minj = std::min(minj, result);
            }
            child->setWeight(minj);
            weightSum += minj;
            if (_syncAnimations && child->animTrack()) {
                weightedDurationSum += child->animTrack()->duration() / child->absoluteSpeed() * child->rawWeight();
            }
        }
        for (const auto& child : _children) {
            if (weightSum > 0.0f) {
                child->setWeight(child->rawWeight() / weightSum);
            }
            if (_syncAnimations && weightedDurationSum > 0.0f && child->animTrack()) {
                child->setWeightedSpeed(
                    child->animTrack()->duration() / child->absoluteSpeed() / weightedDurationSum);
            }
        }
    }

    // ── 2D directional ──────────────────────────────────────────────────

    const Vector2& AnimBlendTreeDirectional2D::pointCache(const size_t i, const size_t j) const
    {
        const auto key = pointKey(i, j);
        auto it = _pointCache.find(key);
        if (it == _pointCache.end()) {
            const float li = _children[i]->pointLength();
            const float lj = _children[j]->pointLength();
            it = _pointCache.emplace(key, Vector2(
                (lj - li) / ((lj + li) / 2.0f),
                angleRad(_children[i]->point(), _children[j]->point()) * 2.0f)).first;
        }
        return it->second;
    }

    void AnimBlendTreeDirectional2D::calculateWeights()
    {
        if (updateParameterValues() || _children.empty()) {
            return;
        }
        const Vector2 p(_parameterValues.size() > 0 ? _parameterValues[0] : 0.0f,
                        _parameterValues.size() > 1 ? _parameterValues[1] : 0.0f);
        const float pLength = p.length();
        float weightSum = 0.0f;
        float weightedDurationSum = 0.0f;
        for (size_t i = 0; i < _children.size(); ++i) {
            AnimNode* child = _children[i].get();
            const float piLength = child->pointLength();
            float minj = std::numeric_limits<float>::max();
            for (size_t j = 0; j < _children.size(); ++j) {
                if (i == j) continue;
                const Vector2& pipj = pointCache(i, j);
                const float pjLength = _children[j]->pointLength();
                const Vector2 pip(
                    (pLength - piLength) / ((pjLength + piLength) / 2.0f),
                    angleRad(child->point(), p) * 2.0f);
                const float lengthSq = pipj.dot(pipj);
                const float result = lengthSq > 0.0f
                    ? std::clamp(1.0f - std::abs(pip.dot(pipj) / lengthSq), 0.0f, 1.0f) : 0.0f;
                minj = std::min(minj, result);
            }
            child->setWeight(minj);
            weightSum += minj;
            if (_syncAnimations && child->animTrack()) {
                weightedDurationSum += child->animTrack()->duration() / child->absoluteSpeed() * child->rawWeight();
            }
        }
        for (const auto& child : _children) {
            if (weightSum > 0.0f) {
                child->setWeight(child->rawWeight() / weightSum);
            }
            if (_syncAnimations && weightedDurationSum > 0.0f && child->animTrack()) {
                const float weightedChildDuration =
                    child->animTrack()->duration() / weightedDurationSum * weightSum;
                child->setWeightedSpeed(child->absoluteSpeed() * weightedChildDuration);
            }
        }
    }

    // ── Direct ──────────────────────────────────────────────────────────

    void AnimBlendTreeDirect::calculateWeights()
    {
        if (updateParameterValues() || _children.empty()) {
            return;
        }
        float weightSum = 0.0f;
        float weightedDurationSum = 0.0f;
        for (size_t i = 0; i < _children.size(); ++i) {
            weightSum += std::max(i < _parameterValues.size() ? _parameterValues[i] : 0.0f, 0.0f);
            if (_syncAnimations && _children[i]->animTrack()) {
                weightedDurationSum += _children[i]->animTrack()->duration()
                    / _children[i]->absoluteSpeed() * _children[i]->rawWeight();
            }
        }
        for (size_t i = 0; i < _children.size(); ++i) {
            AnimNode* child = _children[i].get();
            const float weight = std::max(i < _parameterValues.size() ? _parameterValues[i] : 0.0f, 0.0f);
            if (weightSum > 0.0f) {
                child->setWeight(weight / weightSum);
                if (_syncAnimations && weightedDurationSum > 0.0f && child->animTrack()) {
                    child->setWeightedSpeed(
                        child->animTrack()->duration() / child->absoluteSpeed() / weightedDurationSum);
                }
            } else {
                child->setWeight(0.0f);
                if (_syncAnimations) {
                    child->setWeightedSpeed(0.0f);
                }
            }
        }
    }
}
