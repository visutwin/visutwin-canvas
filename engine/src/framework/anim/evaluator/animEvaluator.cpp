// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "animEvaluator.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "scene/graphNode.h"

namespace visutwin::canvas
{
    AnimEvaluator::AnimEvaluator(std::unique_ptr<AnimBinder> binder) : _binder(std::move(binder))
    {
    }

    void AnimEvaluator::addClip(const std::shared_ptr<AnimClip>& clip)
    {
        if (!clip) {
            return;
        }
        _clips.push_back(clip);
    }

    void AnimEvaluator::removeClip(const size_t index)
    {
        if (index >= _clips.size()) {
            return;
        }
        _clips.erase(_clips.begin() + static_cast<std::ptrdiff_t>(index));
    }

    void AnimEvaluator::removeClips()
    {
        _clips.clear();
    }

    Vector3 AnimEvaluator::lerpVec3(const Vector3& a, const Vector3& b, const float alpha)
    {
        return a + (b - a) * alpha;
    }

    Quaternion AnimEvaluator::slerpQuat(const Quaternion& a, const Quaternion& b, const float alpha)
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

        return Quaternion(
            scale0 * ax + scale1 * bx,
            scale0 * ay + scale1 * by,
            scale0 * az + scale1 * bz,
            scale0 * aw + scale1 * bw).normalized();
    }

    void AnimEvaluator::update(const float dt)
    {
        if (!_binder || _clips.empty()) {
            return;
        }

        for (const auto& clip : _clips) {
            if (clip) {
                clip->update(dt);
            }
        }

        _tmpA.clear();
        _tmpB.clear();

        if (_clips[0]) {
            _clips[0]->eval(_tmpA);
        }

        float blendWeight = 1.0f;
        if (_clips.size() > 1 && _clips[1]) {
            _clips[1]->eval(_tmpB);
            blendWeight = std::clamp(_clips[1]->blendWeight(), 0.0f, 1.0f);
        }

        std::set<std::string> allNodes;
        for (const auto& [name, _] : _tmpA) {
            (void)_;
            allNodes.insert(name);
        }
        for (const auto& [name, _] : _tmpB) {
            (void)_;
            allNodes.insert(name);
        }

        for (const auto& nodeName : allNodes) {
            GraphNode* node = _binder->resolve(nodeName);
            if (!node) {
                continue;
            }

            const auto aIt = _tmpA.find(nodeName);
            const auto bIt = _tmpB.find(nodeName);

            if (bIt == _tmpB.end()) {
                const auto& a = aIt->second;
                if (a.hasPosition) {
                    node->setLocalPosition(a.position);
                }
                if (a.hasRotation) {
                    node->setLocalRotation(a.rotation);
                }
                if (a.hasScale) {
                    node->setLocalScale(a.scale);
                }
                continue;
            }

            if (aIt == _tmpA.end()) {
                const auto& b = bIt->second;
                if (b.hasPosition) {
                    node->setLocalPosition(b.position);
                }
                if (b.hasRotation) {
                    node->setLocalRotation(b.rotation);
                }
                if (b.hasScale) {
                    node->setLocalScale(b.scale);
                }
                continue;
            }

            const auto& a = aIt->second;
            const auto& b = bIt->second;

            if (a.hasPosition || b.hasPosition) {
                node->setLocalPosition(lerpVec3(a.position, b.position, blendWeight));
            }
            if (a.hasRotation || b.hasRotation) {
                node->setLocalRotation(slerpQuat(a.rotation, b.rotation, blendWeight));
            }
            if (a.hasScale || b.hasScale) {
                node->setLocalScale(lerpVec3(a.scale, b.scale, blendWeight));
            }
        }
    }
}
