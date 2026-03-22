// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "animation.h"

namespace visutwin::canvas
{
    AnimationNode* Animation::getNode(const std::string& nodeName)
    {
        const auto it = _nodeDict.find(nodeName);
        if (it == _nodeDict.end()) {
            return nullptr;
        }

        return &_nodes[it->second];
    }

    const AnimationNode* Animation::getNode(const std::string& nodeName) const
    {
        const auto it = _nodeDict.find(nodeName);
        if (it == _nodeDict.end()) {
            return nullptr;
        }

        return &_nodes[it->second];
    }

    void Animation::addNode(const AnimationNode& node)
    {
        _nodeDict[node.name()] = _nodes.size();
        _nodes.push_back(node);
    }
}
