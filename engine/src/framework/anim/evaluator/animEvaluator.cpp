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

    const AnimEvaluator::TrackBinding& AnimEvaluator::bindingFor(const AnimTrack& track)
    {
        TrackBinding& binding = _trackBindings[track.serial()];
        const auto& targets = track.targets();
        // Targets are only ever appended (AnimTrack::addCurve), so a binding made
        // earlier is extended rather than rebuilt.
        while (binding.slots.size() < targets.size()) {
            const std::string& path = targets[binding.slots.size()];
            const auto [it, inserted] = _slotLookup.try_emplace(path, _slots.size());
            if (inserted) {
                _slots.emplace_back();
                _slotPaths.push_back(path);
            }
            binding.slots.push_back(it->second);
        }
        return binding;
    }

    GraphNode* AnimEvaluator::nodeFor(Slot& slot, const std::string& path)
    {
        if (slot.resolvedVersion != _binder->version()) {
            slot.node = _binder->resolve(path);
            slot.resolvedVersion = _binder->version();
        }
        return slot.node;
    }

    const std::vector<MorphInstance*>& AnimEvaluator::morphsFor(Slot& slot, const std::string& path)
    {
        if (slot.morphsVersion != _binder->version()) {
            slot.morphs = _binder->resolveMorphInstances(path);
            slot.morphsVersion = _binder->version();
        }
        return slot.morphs;
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
        //
        // Per node the accumulation lives in a persistent SLOT, and each clip's track
        // evaluates into a reused array indexed by its own targets, so an update hashes
        // no node path and allocates nothing per animated node.
        _touchedSlots.clear();

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
            const AnimTrack* track = clip->track().get();
            if (!track) {
                continue;
            }

            const TrackBinding& binding = bindingFor(*track);
            const size_t targetCount = track->targets().size();
            if (_scratch.size() < targetCount) {
                _scratch.resize(targetCount);
            }
            _scratchTouched.assign(targetCount, 0);
            for (size_t t = 0; t < targetCount; ++t) {
                AnimTransform& entry = _scratch[t];
                entry.hasPosition = entry.hasRotation = entry.hasScale = entry.hasWeights = false;
            }
            clip->eval(_scratch.data(), _scratchTouched.data());

            for (size_t t = 0; t < targetCount; ++t) {
                if (!_scratchTouched[t]) {
                    continue;
                }
                const AnimTransform& transform = _scratch[t];
                const size_t slotIndex = binding.slots[t];
                Slot& acc = _slots[slotIndex];
                if (!acc.touched) {
                    // The first contribution this update: reset the slot's counters and flags.
                    acc.touched = true;
                    acc.posCounter = acc.rotCounter = acc.sclCounter = acc.wgtCounter = 0;
                    acc.value.hasPosition = acc.value.hasRotation = acc.value.hasScale = false;
                    acc.value.hasWeights = false;
                    _touchedSlots.push_back(slotIndex);
                }
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
            for (const size_t slotIndex : _touchedSlots) {
                _slots[slotIndex].touched = false;
                _poseSink(slotIndex, _slotPaths[slotIndex], _slots[slotIndex].value);
            }
            return;
        }

        for (const size_t slotIndex : _touchedSlots) {
            Slot& acc = _slots[slotIndex];
            GraphNode* node = nodeFor(acc, _slotPaths[slotIndex]);
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
        for (const size_t slotIndex : _touchedSlots) {
            Slot& acc = _slots[slotIndex];
            acc.touched = false;
            if (!acc.value.hasWeights) {
                continue;
            }
            for (auto* morphInstance : morphsFor(acc, _slotPaths[slotIndex])) {
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
