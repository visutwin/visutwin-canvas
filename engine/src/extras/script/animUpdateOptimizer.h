// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.11.2026
//
// Lowers the CPU cost of an animated character by steering its anim component's
// offscreenUpdate and evaluationInterval. Add it to the entity carrying the AnimComponent.
//
// - Visibility prediction: the anim component judges its visibility from the previous frame,
//   so a character coming into view would show its last pose for one frame. The script tests
//   the character's bounds (grown by `margin`) against a camera's frustum BEFORE the animation
//   update, and sets FULL on a character likely to be seen, `offscreenUpdate` otherwise.
// - Distance level of detail: the further the character is from the camera, the less often its
//   animation is evaluated: every frame within `lodDistance`, every other frame up to twice
//   that, and so on, up to `maxEvaluationInterval`. A distance of 0 turns it off.
//
// The bounds are the render components' custom AABB, which is also what the renderer culls them
// with; without one, the mesh instances' bounds are used, which for a skinned mesh means the
// bone transforms every frame (warned once). The work runs in update(), as the options must be
// set before the animation update, which runs between update and postUpdate; for the prediction
// to see this frame's camera, whatever moves the camera must run first (otherwise `margin`
// covers a small movement). Each camera's frustum is built once a frame and shared by every
// script using it. The render components are cached; the cache is dropped when a render
// component is added to or removed from the engine, and refilled while it is empty.
//
#pragma once

#include <vector>

#include "framework/components/anim/animComponent.h"
#include "framework/destroyWatch.h"
#include "framework/script/script.h"
#include "framework/script/scriptRegistry.h"

namespace visutwin::canvas
{
    class Entity;
    class RenderComponent;

    class AnimUpdateOptimizer : public Script
    {
    public:
        SCRIPT_NAME("AnimUpdateOptimizer")

        AnimUpdateOptimizer();
        ~AnimUpdateOptimizer() override;

        void initialize() override;
        void update(float dt) override;
        void cloneFrom(const Script& source) override;
        void resolveClonedReferences(const Script& source, const CloneNodeMap& map) override;

        /// The entity with the camera component the character is optimised for.
        [[nodiscard]] Entity* camera() const { return _camera; }
        void setCamera(Entity* camera);

        /// Whether to predict the visibility. Off, the anim component takes `offscreenUpdate`
        /// and judges its visibility alone.
        [[nodiscard]] bool predict() const { return _predict; }
        void setPredict(const bool value) { _predict = value; }

        /// Added to the bounds, so a character just outside the view, which a moving camera may
        /// bring into it, is predicted visible too.
        [[nodiscard]] float margin() const { return _margin; }
        void setMargin(const float value) { _margin = value; }

        /// How the anim component updates while the character is not predicted to be visible.
        [[nodiscard]] AnimOffscreenUpdate offscreenUpdate() const { return _offscreenUpdate; }
        void setOffscreenUpdate(const AnimOffscreenUpdate value) { _offscreenUpdate = value; }

        /// The distance from the camera per step of the evaluation interval; 0 evaluates every frame.
        [[nodiscard]] float lodDistance() const { return _lodDistance; }
        void setLodDistance(const float value) { _lodDistance = value; }

        /// The largest evaluation interval, for the character furthest from the camera.
        [[nodiscard]] int maxEvaluationInterval() const { return _maxEvaluationInterval; }
        void setMaxEvaluationInterval(const int value) { _maxEvaluationInterval = value < 1 ? 1 : value; }

        /// True when the character was predicted visible in the last update.
        [[nodiscard]] bool visible() const { return _visible; }

    private:
        bool predictVisible(Entity* cameraEntity);
        void restoreDefaults();

        Entity* _camera = nullptr;
        DestroyWatch _cameraWatch;
        bool _predict = true;
        float _margin = 1.0f;
        AnimOffscreenUpdate _offscreenUpdate = AnimOffscreenUpdate::STATE;
        float _lodDistance = 0.0f;
        int _maxEvaluationInterval = 4;
        bool _visible = false;

        std::vector<RenderComponent*> _renders;
        EventHandlePtr _renderAddHandle;
        EventHandlePtr _renderRemoveHandle;
    };
}
