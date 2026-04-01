// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "animationComponent.h"

#include "framework/anim/binder/defaultAnimBinder.h"
#include "framework/entity.h"
#include "scene/graphNode.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    AnimationComponent::AnimationComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    AnimationComponent::~AnimationComponent()
    {
        const auto it = std::find(_instances.begin(), _instances.end(), this);
        if (it != _instances.end()) {
            _instances.erase(it);
        }
    }

    void AnimationComponent::setAnimations(const std::unordered_map<std::string, AnimationResource>& value)
    {
        _animations = value;
        onSetAnimations();
    }

    void AnimationComponent::addAnimation(const std::string& name, const std::shared_ptr<Animation>& animation)
    {
        _animations[name] = animation;
        onSetAnimations();
    }

    void AnimationComponent::addAnimation(const std::string& name, const std::shared_ptr<AnimTrack>& animationTrack)
    {
        _animations[name] = animationTrack;
        onSetAnimations();
    }

    void AnimationComponent::setCurrentTime(const float currentTime)
    {
        if (_skeleton) {
            _skeleton->setCurrentTime(currentTime);
        }

        if (_animEvaluator) {
            for (auto& clip : _animEvaluator->clips()) {
                if (clip) {
                    clip->setTime(currentTime);
                }
            }
        }
    }

    float AnimationComponent::currentTime() const
    {
        if (_skeleton) {
            return _skeleton->currentTime();
        }

        if (_animEvaluator) {
            const auto& clips = _animEvaluator->clips();
            if (!clips.empty() && clips.back()) {
                return clips.back()->time();
            }
        }

        return 0.0f;
    }

    float AnimationComponent::duration() const
    {
        if (_currAnim.empty()) {
            spdlog::warn("No animation is playing to get a duration. Returning 0.");
            return 0.0f;
        }

        if (const auto animation = getCurrentAnimation()) {
            return animation->duration();
        }

        if (const auto track = getCurrentAnimTrack()) {
            return track->duration();
        }

        return 0.0f;
    }

    void AnimationComponent::setLoop(const bool value)
    {
        _loop = value;

        if (_skeleton) {
            _skeleton->setLooping(value);
        }

        if (_animEvaluator) {
            for (auto& clip : _animEvaluator->clips()) {
                if (clip) {
                    clip->setLoop(value);
                }
            }
        }
    }

    void AnimationComponent::setModel(GraphNode* model)
    {
        if (model == _model) {
            return;
        }

        resetAnimationController();
        _model = model;

        if (!_currAnim.empty() && _animations.contains(_currAnim)) {
            play(_currAnim);
        }
    }

    std::shared_ptr<Animation> AnimationComponent::getAnimation(const std::string& name) const
    {
        const auto it = _animations.find(name);
        if (it == _animations.end()) {
            return nullptr;
        }

        if (const auto* animation = std::get_if<std::shared_ptr<Animation>>(&it->second)) {
            return *animation;
        }

        return nullptr;
    }

    std::shared_ptr<AnimTrack> AnimationComponent::getAnimTrack(const std::string& name) const
    {
        const auto it = _animations.find(name);
        if (it == _animations.end()) {
            return nullptr;
        }

        if (const auto* animationTrack = std::get_if<std::shared_ptr<AnimTrack>>(&it->second)) {
            return *animationTrack;
        }

        return nullptr;
    }

    std::shared_ptr<Animation> AnimationComponent::getCurrentAnimation() const
    {
        return getAnimation(_currAnim);
    }

    std::shared_ptr<AnimTrack> AnimationComponent::getCurrentAnimTrack() const
    {
        return getAnimTrack(_currAnim);
    }

    void AnimationComponent::play(const std::string& name, const float blendTime)
    {
        if (!enabled() || !_entity || !_entity->enabled()) {
            return;
        }

        if (!_animations.contains(name)) {
            spdlog::error("Trying to play animation '{}' which doesn't exist", name);
            return;
        }

        _prevAnim = _currAnim;
        _currAnim = name;

        if (_model) {
            if (!_skeleton && !_animEvaluator) {
                createAnimationController();
            }

            _blending = blendTime > 0.0f && !_prevAnim.empty();
            if (_blending) {
                _blend = 0.0f;
                _blendSpeed = 1.0f / blendTime;
            }

            if (_skeleton) {
                if (_blending) {
                    if (const auto prevAnim = getAnimation(_prevAnim)) {
                        _fromSkel->setAnimation(prevAnim.get());
                        _fromSkel->addTime(_skeleton->currentTime());
                    }
                    if (const auto currAnim = getAnimation(_currAnim)) {
                        _toSkel->setAnimation(currAnim.get());
                    }
                } else if (const auto currAnim = getAnimation(_currAnim)) {
                    _skeleton->setAnimation(currAnim.get());
                }
            }

            if (_animEvaluator) {
                auto& clips = _animEvaluator->clips();
                if (_blending) {
                    while (clips.size() > 1) {
                        _animEvaluator->removeClip(0);
                    }
                } else {
                    _animEvaluator->removeClips();
                }

                if (const auto track = getAnimTrack(_currAnim)) {
                    auto clip = std::make_shared<AnimClip>(track, 0.0f, 1.0f, true, _loop);
                    clip->setName(_currAnim);
                    clip->setBlendWeight(_blending ? 0.0f : 1.0f);
                    clip->reset();
                    _animEvaluator->addClip(clip);
                }
            }
        }

        _playing = true;
    }

    void AnimationComponent::onSetAnimations()
    {
        if (_entity && !_model) {
            // DEVIATION: ModelComponent is not yet fully implemented,
            // so the animation component binds to the entity graph root.
            setModel(_entity);
        }

        if (_currAnim.empty() && _activate && enabled() && _entity && _entity->enabled()) {
            if (!_animations.empty()) {
                play(_animations.begin()->first);
            }
        }
    }

    void AnimationComponent::resetAnimationController()
    {
        _skeleton.reset();
        _fromSkel.reset();
        _toSkel.reset();

        _animEvaluator.reset();
    }

    void AnimationComponent::createAnimationController()
    {
        bool hasJson = false;
        bool hasGlb = false;

        for (const auto& [_, animation] : _animations) {
            if (std::holds_alternative<std::shared_ptr<AnimTrack>>(animation)) {
                hasGlb = true;
            } else {
                hasJson = true;
            }
        }

        if (hasJson) {
            _fromSkel = std::make_unique<Skeleton>(_model);
            _toSkel = std::make_unique<Skeleton>(_model);
            _skeleton = std::make_unique<Skeleton>(_model);
            _skeleton->setLooping(_loop);
            _skeleton->setGraph(_model);
        } else if (hasGlb) {
            _animEvaluator = std::make_unique<AnimEvaluator>(std::make_unique<DefaultAnimBinder>(_entity));
        }
    }

    void AnimationComponent::stopCurrentAnimation()
    {
        _currAnim.clear();

        _playing = false;
        if (_skeleton) {
            _skeleton->setCurrentTime(0.0f);
            _skeleton->setAnimation(nullptr);
        }
        if (_animEvaluator) {
            for (auto& clip : _animEvaluator->clips()) {
                if (clip) {
                    clip->stop();
                }
            }
            _animEvaluator->update(0.0f);
            _animEvaluator->removeClips();
        }
    }

    void AnimationComponent::onEnable()
    {
        Component::onEnable();

        if (_activate && _currAnim.empty() && !_animations.empty()) {
            play(_animations.begin()->first);
        }
    }

    void AnimationComponent::onBeforeRemove()
    {
        _skeleton.reset();
        _fromSkel.reset();
        _toSkel.reset();

        _animEvaluator.reset();
    }

    void AnimationComponent::update(const float dt)
    {
        float blendAlpha = _blend;

        if (_blending) {
            _blend += dt * _blendSpeed;
            if (_blend >= 1.0f) {
                _blend = 1.0f;
            }

            blendAlpha = _blend;
            if (_hasBlendCurve) {
                // DEVIATION: upstream AnimationComponent blends linearly; this implementation allows optional curve remap.
                blendAlpha = std::clamp(_blendCurve.value(_blend), 0.0f, 1.0f);
            }
        }

        if (_playing) {
            if (_skeleton && _model) {
                if (_blending) {
                    _skeleton->blend(_fromSkel.get(), _toSkel.get(), blendAlpha);
                } else {
                    const float delta = dt * _speed;
                    _skeleton->addTime(delta);
                    if (_speed > 0.0f && (_skeleton->currentTime() == duration()) && !_loop) {
                        _playing = false;
                    } else if (_speed < 0.0f && _skeleton->currentTime() == 0.0f && !_loop) {
                        _playing = false;
                    }
                }

                if (_blending && (_blend == 1.0f)) {
                    _skeleton->setAnimation(_toSkel->animation());
                }

                _skeleton->updateGraph();
            }
        }

        if (_animEvaluator) {
            for (auto& clip : _animEvaluator->clips()) {
                if (!clip) {
                    continue;
                }

                clip->setSpeed(_speed);
                if (!_playing) {
                    clip->pause();
                } else {
                    clip->resume();
                }
            }

            if (_blending && _animEvaluator->clips().size() > 1 && _animEvaluator->clips()[1]) {
                _animEvaluator->clips()[1]->setBlendWeight(blendAlpha);
            }

            _animEvaluator->update(dt);
        }

        if (_blending && _blend == 1.0f) {
            _blending = false;
        }
    }

    void AnimationComponent::setBlendCurve(const Curve& curve)
    {
        _blendCurve = curve;
        _hasBlendCurve = true;
    }

    void AnimationComponent::clearBlendCurve()
    {
        _hasBlendCurve = false;
    }
}
