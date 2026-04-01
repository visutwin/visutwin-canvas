// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "skeleton.h"

#include <algorithm>
#include <cmath>

namespace visutwin::canvas
{
    namespace
    {
        void addInterpolatedKeysRecursive(GraphNode* node,
                                          std::vector<InterpolatedKey>& interpolatedKeys,
                                          std::unordered_map<std::string, size_t>& interpolatedKeyDict,
                                          std::unordered_map<std::string, int>& currKeyIndices)
        {
            if (!node) {
                return;
            }

            InterpolatedKey interpKey;
            interpKey.name = node->name();
            interpolatedKeyDict[interpKey.name] = interpolatedKeys.size();
            currKeyIndices[interpKey.name] = 0;
            interpolatedKeys.push_back(interpKey);

            for (auto* child : node->children()) {
                addInterpolatedKeysRecursive(child, interpolatedKeys, interpolatedKeyDict, currKeyIndices);
            }
        }
    }

    Skeleton::Skeleton(GraphNode* graph)
    {
        addInterpolatedKeysRecursive(graph, _interpolatedKeys, _interpolatedKeyDict, _currKeyIndices);
    }

    void Skeleton::setAnimation(Animation* value)
    {
        _animation = value;
        setCurrentTime(0.0f);
    }

    void Skeleton::setCurrentTime(const float value)
    {
        _time = value;

        for (const auto& key : _interpolatedKeys) {
            _currKeyIndices[key.name] = 0;
        }

        addTime(0.0f);
        updateGraph();
    }

    Vector3 Skeleton::lerpVec3(const Vector3& a, const Vector3& b, const float alpha)
    {
        return a + (b - a) * alpha;
    }

    Quaternion Skeleton::slerpQuat(const Quaternion& a, const Quaternion& b, const float alpha)
    {
        float ax = a.getX();
        float ay = a.getY();
        float az = a.getZ();
        float aw = a.getW();

        float bx = b.getX();
        float by = b.getY();
        float bz = b.getZ();
        float bw = b.getW();

        float dot = ax * bx + ay * by + az * bz + aw * bw;

        if (dot < 0.0f) {
            bx = -bx;
            by = -by;
            bz = -bz;
            bw = -bw;
            dot = -dot;
        }

        constexpr float epsilon = 1e-6f;
        float scale0 = 1.0f - alpha;
        float scale1 = alpha;

        if ((1.0f - dot) > epsilon) {
            const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
            const float invSinTheta = 1.0f / std::sin(theta);
            scale0 = std::sin((1.0f - alpha) * theta) * invSinTheta;
            scale1 = std::sin(alpha * theta) * invSinTheta;
        }

        Quaternion result(
            scale0 * ax + scale1 * bx,
            scale0 * ay + scale1 * by,
            scale0 * az + scale1 * bz,
            scale0 * aw + scale1 * bw);

        return result.normalized();
    }

    void Skeleton::addTime(const float delta)
    {
        if (!_animation) {
            return;
        }

        const auto& nodes = _animation->nodes();
        const float duration = _animation->duration();

        if ((_time == duration) && !_looping) {
            return;
        }

        _time += delta;

        if (_time > duration) {
            _time = _looping ? 0.0f : duration;
            for (const auto& node : nodes) {
                _currKeyIndices[node.name()] = 0;
            }
        } else if (_time < 0.0f) {
            _time = _looping ? duration : 0.0f;
            for (const auto& node : nodes) {
                _currKeyIndices[node.name()] = static_cast<int>(node.keys().size()) - 2;
            }
        }

        const int offset = (delta >= 0.0f ? 1 : -1);

        for (const auto& node : nodes) {
            const std::string& nodeName = node.name();
            const auto interpIt = _interpolatedKeyDict.find(nodeName);
            if (interpIt == _interpolatedKeyDict.end()) {
                continue;
            }

            InterpolatedKey& interpKey = _interpolatedKeys[interpIt->second];
            const auto& keys = node.keys();
            if (keys.empty()) {
                continue;
            }

            bool foundKey = false;
            if (keys.size() != 1) {
                int currKeyIndex = _currKeyIndices[nodeName];
                for (; currKeyIndex < static_cast<int>(keys.size()) - 1 && currKeyIndex >= 0; currKeyIndex += offset) {
                    const auto& k1 = keys[currKeyIndex];
                    const auto& k2 = keys[currKeyIndex + 1];

                    if ((k1.time <= _time) && (k2.time >= _time)) {
                        const float alpha = (k2.time > k1.time) ? ((_time - k1.time) / (k2.time - k1.time)) : 0.0f;

                        interpKey.pos = lerpVec3(k1.position, k2.position, alpha);
                        interpKey.quat = slerpQuat(k1.rotation, k2.rotation, alpha);
                        interpKey.scale = lerpVec3(k1.scale, k2.scale, alpha);
                        interpKey.written = true;

                        _currKeyIndices[nodeName] = currKeyIndex;
                        foundKey = true;
                        break;
                    }
                }
            }

            if (keys.size() == 1 || (!foundKey && _time == 0.0f && _looping)) {
                interpKey.pos = keys[0].position;
                interpKey.quat = keys[0].rotation;
                interpKey.scale = keys[0].scale;
                interpKey.written = true;
            }
        }
    }

    void Skeleton::blend(const Skeleton* skel1, const Skeleton* skel2, const float alpha)
    {
        if (!skel1 || !skel2) {
            return;
        }

        const size_t numNodes = std::min(_interpolatedKeys.size(), std::min(skel1->_interpolatedKeys.size(), skel2->_interpolatedKeys.size()));
        for (size_t i = 0; i < numNodes; i++) {
            const auto& key1 = skel1->_interpolatedKeys[i];
            const auto& key2 = skel2->_interpolatedKeys[i];
            auto& dstKey = _interpolatedKeys[i];

            if (key1.written && key2.written) {
                dstKey.quat = slerpQuat(key1.quat, key2.quat, alpha);
                dstKey.pos = lerpVec3(key1.pos, key2.pos, alpha);
                dstKey.scale = lerpVec3(key1.scale, key2.scale, alpha);
                dstKey.written = true;
            } else if (key1.written) {
                dstKey.quat = key1.quat;
                dstKey.pos = key1.pos;
                dstKey.scale = key1.scale;
                dstKey.written = true;
            } else if (key2.written) {
                dstKey.quat = key2.quat;
                dstKey.pos = key2.pos;
                dstKey.scale = key2.scale;
                dstKey.written = true;
            }
        }
    }

    void Skeleton::setGraph(GraphNode* graph)
    {
        _graph = graph;

        if (_graph) {
            for (auto& interpKey : _interpolatedKeys) {
                interpKey.setTarget(_graph->findByName(interpKey.name));
            }
        } else {
            for (auto& interpKey : _interpolatedKeys) {
                interpKey.setTarget(nullptr);
            }
        }
    }

    void Skeleton::updateGraph()
    {
        if (!_graph) {
            return;
        }

        for (auto& interpKey : _interpolatedKeys) {
            if (!interpKey.written) {
                continue;
            }

            GraphNode* transform = interpKey.getTarget();
            if (!transform) {
                continue;
            }

            transform->setLocalPosition(interpKey.pos);
            transform->setLocalRotation(interpKey.quat);
            transform->setLocalScale(interpKey.scale);

            interpKey.written = false;
        }
    }
}
