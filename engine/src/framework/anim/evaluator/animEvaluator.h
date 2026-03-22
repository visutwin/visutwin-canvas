// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "framework/anim/binder/animBinder.h"
#include "animClip.h"

namespace visutwin::canvas
{
    class AnimEvaluator
    {
    public:
        explicit AnimEvaluator(std::unique_ptr<AnimBinder> binder);

        const std::vector<std::shared_ptr<AnimClip>>& clips() const { return _clips; }
        std::vector<std::shared_ptr<AnimClip>>& clips() { return _clips; }

        void addClip(const std::shared_ptr<AnimClip>& clip);
        void removeClip(size_t index);
        void removeClips();

        void update(float dt);

    private:
        static Vector3 lerpVec3(const Vector3& a, const Vector3& b, float alpha);
        static Quaternion slerpQuat(const Quaternion& a, const Quaternion& b, float alpha);

        std::unique_ptr<AnimBinder> _binder;
        std::vector<std::shared_ptr<AnimClip>> _clips;

        std::unordered_map<std::string, AnimTransform> _tmpA;
        std::unordered_map<std::string, AnimTransform> _tmpB;
    };
}
