// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "animation.h"
#include "scene/graphNode.h"

namespace visutwin::canvas
{
    class InterpolatedKey
    {
    public:
        bool written = false;
        std::string name;
        Quaternion quat;
        Vector3 pos;
        Vector3 scale;

        GraphNode* getTarget() const { return _targetNode; }
        void setTarget(GraphNode* node) { _targetNode = node; }

    private:
        GraphNode* _targetNode = nullptr;
    };

    class Skeleton
    {
    public:
        explicit Skeleton(GraphNode* graph);

        void setAnimation(Animation* value);
        Animation* animation() const { return _animation; }

        void setCurrentTime(float value);
        float currentTime() const { return _time; }

        int numNodes() const { return static_cast<int>(_interpolatedKeys.size()); }

        void addTime(float delta);
        void blend(const Skeleton* skel1, const Skeleton* skel2, float alpha);
        void setGraph(GraphNode* graph);
        void updateGraph();

        void setLooping(bool value) { _looping = value; }
        bool looping() const { return _looping; }

    private:
        bool _looping = true;

        Animation* _animation = nullptr;
        float _time = 0.0f;

        std::vector<InterpolatedKey> _interpolatedKeys;
        std::unordered_map<std::string, size_t> _interpolatedKeyDict;
        std::unordered_map<std::string, int> _currKeyIndices;

        GraphNode* _graph = nullptr;

        static Vector3 lerpVec3(const Vector3& a, const Vector3& b, float alpha);
        static Quaternion slerpQuat(const Quaternion& a, const Quaternion& b, float alpha);
    };
}
