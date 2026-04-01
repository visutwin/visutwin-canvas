// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "framework/components/component.h"
#include "framework/anim/evaluator/animClip.h"
#include "framework/anim/evaluator/animEvaluator.h"
#include "core/math/curveSet.h"
#include "scene/animation/animation.h"
#include "scene/animation/skeleton.h"

namespace visutwin::canvas
{
    class GraphNode;

    class AnimationComponent : public Component
    {
    public:
        using AnimationResource = std::variant<std::shared_ptr<Animation>, std::shared_ptr<AnimTrack>>;

        AnimationComponent(IComponentSystem* system, Entity* entity);
        ~AnimationComponent() override;

        void initializeComponentData() override {}

        static const std::vector<AnimationComponent*>& instances() { return _instances; }

        void setAnimations(const std::unordered_map<std::string, AnimationResource>& value);
        const std::unordered_map<std::string, AnimationResource>& animations() const { return _animations; }

        void setAssets(const std::vector<int>& value) { _assets = value; }
        const std::vector<int>& assets() const { return _assets; }

        void setCurrentTime(float currentTime);
        float currentTime() const;

        float duration() const;

        void setLoop(bool value);
        bool loop() const { return _loop; }

        void setModel(GraphNode* model);

        void addAnimation(const std::string& name, const std::shared_ptr<Animation>& animation);
        void addAnimation(const std::string& name, const std::shared_ptr<AnimTrack>& animationTrack);

        void play(const std::string& name, float blendTime = 0.0f);

        std::shared_ptr<Animation> getAnimation(const std::string& name) const;
        std::shared_ptr<AnimTrack> getAnimTrack(const std::string& name) const;

        void onSetAnimations();
        void onEnable() override;
        void onBeforeRemove();

        void update(float dt);

        // DEVIATION: tool-path workflows in this port require component-level curve remapping for blend alpha.
        void setBlendCurve(const Curve& curve);
        void clearBlendCurve();

        void setActivate(bool value) { _activate = value; }
        bool activate() const { return _activate; }

        void setSpeed(float value) { _speed = value; }
        float speed() const { return _speed; }

        void setPlaying(bool value) { _playing = value; }
        bool playing() const { return _playing; }

    private:
        void resetAnimationController();
        void createAnimationController();
        void stopCurrentAnimation();

        std::shared_ptr<Animation> getCurrentAnimation() const;
        std::shared_ptr<AnimTrack> getCurrentAnimTrack() const;

        bool _activate = true;
        float _speed = 1.0f;
        bool _playing = false;

        inline static std::vector<AnimationComponent*> _instances;

        std::unordered_map<std::string, AnimationResource> _animations;
        std::vector<int> _assets;

        bool _loop = true;

        std::unique_ptr<AnimEvaluator> _animEvaluator;

        GraphNode* _model = nullptr;

        std::unique_ptr<Skeleton> _skeleton;
        std::unique_ptr<Skeleton> _fromSkel;
        std::unique_ptr<Skeleton> _toSkel;

        std::unordered_map<int, std::string> _animationsIndex;

        std::string _prevAnim;
        std::string _currAnim;

        float _blend = 0.0f;
        bool _blending = false;
        float _blendSpeed = 0.0f;

        bool _hasBlendCurve = false;
        Curve _blendCurve;
    };
}
