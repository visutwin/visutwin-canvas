// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "animBlendTree.h"
#include "animNode.h"

namespace visutwin::canvas
{
    class AnimController;

    /**
     * Defines a single state that the controller can be in. Each state contains either a single
     * AnimNode or an AnimBlendTree of multiple AnimNodes which animate the entity while the state
     * is active. A state stays active until a transition with met conditions leaves it.
     */
    class AnimState
    {
    public:
        AnimState(AnimController* controller, std::string name, float speed = 1.0f,
                  bool loop = true, const std::optional<AnimBlendTreeDesc>& blendTree = std::nullopt);

        const std::string& name() const { return _name; }

        float speed() const { return _speed; }
        void setSpeed(const float value) { _speed = value; }

        bool loop() const { return _loop; }
        void setLoop(const bool value) { _loop = value; }

        AnimNode* blendTree() const { return _blendTree.get(); }

        /**
         * Assign an animation track to the node addressed by `path` — the state name,
         * optionally followed by blend-tree node names ("State.TreeNode.Leaf").
         */
        void addAnimation(const std::vector<std::string>& path, const std::shared_ptr<AnimTrack>& track);

        /** Flat list of nodes with assigned animations (non-owning). */
        const std::vector<AnimNode*>& animations() const { return _animationList; }
        void clearAnimations();

        bool hasAnimations() const { return _hasAnimations; }
        bool playable() const;

        int nodeCount() const;
        float totalWeight() const;
        float timelineDuration() const;

    private:
        void updateHasAnimations();
        AnimNode* getNodeFromPath(const std::vector<std::string>& path) const;

        AnimController* _controller;
        std::string _name;
        float _speed;
        bool _loop;
        bool _hasAnimations = false;
        std::unique_ptr<AnimNode> _blendTree;
        std::vector<AnimNode*> _animationList;
    };
}
