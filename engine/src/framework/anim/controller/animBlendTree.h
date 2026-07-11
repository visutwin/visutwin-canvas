// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "animConstants.h"
#include "animNode.h"

namespace visutwin::canvas
{
    /** Callback used at runtime to read the current parameter values. */
    using AnimFindParameterFn = std::function<const AnimParameter*(const std::string&)>;

    /**
     * Declarative description of a blend-tree node, used to build the runtime tree
     * (DEVIATION: replaces upstream's untyped JSON blend-tree objects).
     * A node with children is a blend tree of `type`; a leaf maps one animation.
     */
    struct AnimBlendTreeDesc
    {
        std::string name;
        AnimBlendType type = AnimBlendType::BLEND_1D;
        Vector2 point = Vector2(0.0f, 0.0f);     // leaf position (x used for 1D)
        float speed = 1.0f;                      // leaf playback speed
        std::vector<std::string> parameters;     // driving parameter names (tree nodes)
        std::vector<AnimBlendTreeDesc> children; // empty = leaf
        bool syncAnimations = true;
    };

    /**
     * AnimBlendTrees store and blend multiple AnimNodes together, computing per-child
     * weights from the current parameter values. DEVIATION: the base class and its four
     * weighting variants (1D, 2D cartesian, 2D directional, direct) live in one file
     * instead of upstream's five modules.
     */
    class AnimBlendTree : public AnimNode
    {
    public:
        AnimBlendTree(AnimState* state, AnimBlendTree* parent, const std::string& name,
                      const Vector2& point, std::vector<std::string> parameters,
                      const std::vector<AnimBlendTreeDesc>& children, bool syncAnimations,
                      const AnimFindParameterFn& findParameter);

        /** Recursively build a blend tree (or leaf node) from its description. */
        static std::unique_ptr<AnimNode> create(const AnimBlendTreeDesc& desc, AnimState* state,
            AnimBlendTree* parent, const AnimFindParameterFn& findParameter);

        float weight() const override;
        bool isBlendTree() const override { return true; }
        bool syncAnimations() const override { return _syncAnimations; }

        AnimNode* getChild(const std::string& name) const;
        const std::vector<std::unique_ptr<AnimNode>>& children() const { return _children; }
        int getNodeCount() const;

        /** Recompute child weights from the current parameter values. */
        virtual void calculateWeights() = 0;

    protected:
        /** Refresh cached parameter values; returns true when nothing changed. */
        bool updateParameterValues();

        std::vector<std::string> _parameters;
        mutable std::vector<float> _parameterValues;
        std::vector<std::unique_ptr<AnimNode>> _children;
        AnimFindParameterFn _findParameter;
        bool _syncAnimations;
        mutable std::unordered_map<uint64_t, Vector2> _pointCache;
    };

    /** 1D blend (Rune Skovbo Johansen thesis ch. 6): children pre-sorted by point.x in create(). */
    class AnimBlendTree1D final : public AnimBlendTree
    {
    public:
        using AnimBlendTree::AnimBlendTree;
        void calculateWeights() override;
    };

    /** 2D cartesian gradient-band interpolation. */
    class AnimBlendTreeCartesian2D final : public AnimBlendTree
    {
    public:
        using AnimBlendTree::AnimBlendTree;
        void calculateWeights() override;

    private:
        const Vector2& pointDistanceCache(size_t i, size_t j) const;
    };

    /** 2D directional gradient-band interpolation (polar). */
    class AnimBlendTreeDirectional2D final : public AnimBlendTree
    {
    public:
        using AnimBlendTree::AnimBlendTree;
        void calculateWeights() override;

    private:
        const Vector2& pointCache(size_t i, size_t j) const;
    };

    /** Direct blend: one parameter per child, weights normalized by their sum. */
    class AnimBlendTreeDirect final : public AnimBlendTree
    {
    public:
        using AnimBlendTree::AnimBlendTree;
        void calculateWeights() override;
    };
}
