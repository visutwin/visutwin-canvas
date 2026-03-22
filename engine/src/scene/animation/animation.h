// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/math/quaternion.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    class AnimationKey
    {
    public:
        AnimationKey(float time, const Vector3& position, const Quaternion& rotation, const Vector3& scale)
            : time(time), position(position), rotation(rotation), scale(scale)
        {
        }

        float time;
        Vector3 position;
        Quaternion rotation;
        Vector3 scale;
    };

    class AnimationNode
    {
    public:
        void setName(const std::string& value) { _name = value; }
        const std::string& name() const { return _name; }

        void addKey(const AnimationKey& key) { _keys.push_back(key); }
        std::vector<AnimationKey>& keys() { return _keys; }
        const std::vector<AnimationKey>& keys() const { return _keys; }

    private:
        std::string _name;
        std::vector<AnimationKey> _keys;
    };

    class Animation
    {
    public:
        std::string name;
        float duration = 0.0f;

        AnimationNode* getNode(const std::string& nodeName);
        const AnimationNode* getNode(const std::string& nodeName) const;

        void addNode(const AnimationNode& node);

        std::vector<AnimationNode>& nodes() { return _nodes; }
        const std::vector<AnimationNode>& nodes() const { return _nodes; }

    private:
        std::vector<AnimationNode> _nodes;
        std::unordered_map<std::string, size_t> _nodeDict;
    };
}
