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
                        acc.value.position = Vector3::lerp(acc.value.position, transform.position, weight);
                    }
                    acc.value.hasPosition = true;
                    acc.posCounter++;
                }
                if (transform.hasRotation) {
                    if (acc.rotCounter == 0 || weight >= 1.0f) {
                        acc.value.rotation = transform.rotation;
                    } else {
                        acc.value.rotation = Quaternion::slerp(acc.value.rotation, transform.rotation, weight);
                    }
                    acc.value.hasRotation = true;
                    acc.rotCounter++;
                }
                if (transform.hasScale) {
                    if (acc.sclCounter == 0 || weight >= 1.0f) {
                        acc.value.scale = transform.scale;
                    } else {
                        acc.value.scale = Vector3::lerp(acc.value.scale, transform.scale, weight);
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

        // A component composing several layers takes the pose here and writes nothing
        // itself; see setPoseSink.
        if (_poseSink) {
            for (const auto& [nodeName, acc] : blended) {
                _poseSink(nodeName, acc.value);
            }
            return;
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
