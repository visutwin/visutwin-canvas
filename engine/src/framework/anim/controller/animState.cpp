// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "animState.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "animController.h"

namespace visutwin::canvas
{
    AnimState::AnimState(AnimController* controller, std::string name, const float speed,
        const bool loop, const std::optional<AnimBlendTreeDesc>& blendTree)
        : _controller(controller), _name(std::move(name)), _speed(speed), _loop(loop)
    {
        if (blendTree.has_value()) {
            AnimBlendTreeDesc rootDesc = *blendTree;
            rootDesc.name = _name;
            _blendTree = AnimBlendTree::create(rootDesc, this, nullptr,
                [this](const std::string& parameterName) {
                    return _controller ? _controller->findParameter(parameterName) : nullptr;
                });
        } else {
            _blendTree = std::make_unique<AnimNode>(this, nullptr, _name, Vector2(1.0f, 0.0f), speed);
        }
    }

    AnimNode* AnimState::getNodeFromPath(const std::vector<std::string>& path) const
    {
        AnimNode* currNode = _blendTree.get();
        for (size_t i = 1; i < path.size() && currNode; ++i) {
            if (!currNode->isBlendTree()) {
                return nullptr;
            }
            currNode = static_cast<AnimBlendTree*>(currNode)->getChild(path[i]);
        }
        return currNode;
    }

    void AnimState::addAnimation(const std::vector<std::string>& path,
        const std::shared_ptr<AnimTrack>& track)
    {
        std::string pathString;
        for (const auto& segment : path) {
            if (!pathString.empty()) pathString += ".";
            pathString += segment;
        }
        const auto existing = std::find_if(_animationList.begin(), _animationList.end(),
            [&pathString](const AnimNode* animation) {
                return animation && animation->path() == pathString;
            });
        if (existing != _animationList.end()) {
            (*existing)->setAnimTrack(track);
        } else {
            AnimNode* node = getNodeFromPath(path);
            if (!node) {
                spdlog::error("AnimState '{}': no blend-tree node at path '{}'", _name, pathString);
                return;
            }
            node->setAnimTrack(track);
            _animationList.push_back(node);
        }
        updateHasAnimations();
    }

    void AnimState::clearAnimations()
    {
        _animationList.clear();
        updateHasAnimations();
    }

    void AnimState::updateHasAnimations()
    {
        _hasAnimations = !_animationList.empty() &&
            std::all_of(_animationList.begin(), _animationList.end(),
                [](const AnimNode* animation) { return animation && animation->animTrack(); });
    }

    bool AnimState::playable() const
    {
        return isAnimControlState(_name) ||
            static_cast<int>(_animationList.size()) == nodeCount();
    }

    int AnimState::nodeCount() const
    {
        if (!_blendTree || !_blendTree->isBlendTree()) {
            return 1;
        }
        return static_cast<const AnimBlendTree*>(_blendTree.get())->getNodeCount();
    }

    float AnimState::totalWeight() const
    {
        float sum = 0.0f;
        for (const auto* animation : _animationList) {
            if (animation) {
                sum += animation->weight();
            }
        }
        return sum;
    }

    float AnimState::timelineDuration() const
    {
        float duration = 0.0f;
        for (const auto* animation : _animationList) {
            if (animation && animation->animTrack()) {
                duration = std::max(duration, animation->animTrack()->duration());
            }
        }
        return duration;
    }
}
