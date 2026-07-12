// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "animEvaluator.h"

#include "scene/morphInstance.h"

#include <algorithm>
#include <cmath>

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

    AnimClip* AnimEvaluator::findClip(const std::string& name) const
    {
        for (const auto& clip : _clips) {
            if (clip && clip->name() == name) {
                return clip.get();
            }
        }
        return nullptr;
    }

    void AnimEvaluator::updateClipTrack(const std::string& name, const std::shared_ptr<AnimTrack>& track)
    {
        for (const auto& clip : _clips) {
            if (clip && clip->name().rfind(name, 0) == 0) {
                clip->setTrack(track);
            }
        }
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

        // N-clip sequential blend compositing (mirrors upstream anim-evaluator.js):
        // per node/property, the first contributing clip SETS the value regardless of
        // its weight; each subsequent clip lerps the accumulated value toward its own
        // by its blendWeight. Clips added later (the transition's destination state)
        // therefore composite over earlier ones. A clip with weight >= 1 resets the
        // accumulation. Only clips with weight > 0 advance their time.
        struct Accum
        {
            AnimTransform value;
            int posCounter = 0;
            int rotCounter = 0;
            int sclCounter = 0;
            int wgtCounter = 0;
        };
        std::unordered_map<std::string, Accum> blended;
        std::unordered_map<std::string, AnimTransform> tmp;

        for (const auto& clip : _clips) {
            if (!clip) {
                continue;
            }
            const float weight = std::clamp(clip->blendWeight(), 0.0f, 1.0f);
            if (weight > 0.0f) {
                clip->update(dt);
            } else {
                continue;
            }

            tmp.clear();
            clip->eval(tmp);

            for (const auto& [nodeName, transform] : tmp) {
                auto& acc = blended[nodeName];
                if (transform.hasPosition) {
                    if (acc.posCounter == 0 || weight >= 1.0f) {
                        acc.value.position = transform.position;
                    } else {
                        acc.value.position = lerpVec3(acc.value.position, transform.position, weight);
                    }
                    acc.value.hasPosition = true;
                    acc.posCounter++;
                }
                if (transform.hasRotation) {
                    if (acc.rotCounter == 0 || weight >= 1.0f) {
                        acc.value.rotation = transform.rotation;
                    } else {
                        acc.value.rotation = slerpQuat(acc.value.rotation, transform.rotation, weight);
                    }
                    acc.value.hasRotation = true;
                    acc.rotCounter++;
                }
                if (transform.hasScale) {
                    if (acc.sclCounter == 0 || weight >= 1.0f) {
                        acc.value.scale = transform.scale;
                    } else {
                        acc.value.scale = lerpVec3(acc.value.scale, transform.scale, weight);
                    }
                    acc.value.hasScale = true;
                    acc.sclCounter++;
                }
                if (transform.hasWeights) {
                    if (acc.wgtCounter == 0 || weight >= 1.0f ||
                        acc.value.weights.size() != transform.weights.size()) {
                        acc.value.weights = transform.weights;
                    } else {
                        for (size_t c = 0; c < transform.weights.size(); ++c) {
                            acc.value.weights[c] += (transform.weights[c] - acc.value.weights[c]) * weight;
                        }
                    }
                    acc.value.hasWeights = true;
                    acc.wgtCounter++;
                }
            }
        }

        for (const auto& [nodeName, acc] : blended) {
            GraphNode* node = _binder->resolve(nodeName);
            if (!node) {
                continue;
            }
            if (acc.value.hasPosition) {
                node->setLocalPosition(acc.value.position);
            }
            if (acc.value.hasRotation) {
                node->setLocalRotation(acc.value.rotation);
            }
            if (acc.value.hasScale) {
                node->setLocalScale(acc.value.scale);
            }
        }

        // Morph weight channels: push blended weights into the target node's
        // morph instances (glTF "weights" animation).
        for (const auto& [nodeName, acc] : blended) {
            if (!acc.value.hasWeights) {
                continue;
            }
            for (auto* morphInstance : _binder->resolveMorphInstances(nodeName)) {
                if (!morphInstance) {
                    continue;
                }
                for (size_t c = 0; c < acc.value.weights.size(); ++c) {
                    morphInstance->setWeight(static_cast<int>(c), acc.value.weights[c]);
                }
            }
        }
    }
}
