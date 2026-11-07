// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.11.2026
//
// The anim controller's clock against its exit times. The time in a state advances at the
// playback speed of the state's first CLIP (not the state's speed: a blend tree's children play
// at their own), and the progress is that time over the clip's duration; measured with the
// speed applied twice, a state at speed 2 left on an exit time of 1 after half its clip and
// reported a progress of 2 at its end. Past a non-looping state's end the frame is shortened to
// what the clip takes to reach it, in real time. And a transition out of START or END, which
// play nothing, ignores its exit time: measured against a progress that never moves, only an
// exit time of exactly 1 ever passed and any other left the controller in START for good.
//
// dt and the track duration are exact binary fractions, so the progress lands exactly on the
// exit time instead of drifting past it.

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "framework/anim/binder/animBinder.h"
#include "framework/anim/controller/animController.h"
#include "framework/anim/evaluator/animClip.h"
#include "framework/anim/evaluator/animEvaluator.h"
#include "framework/anim/evaluator/animTrack.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kDt = 1.0f / 16.0f;
    constexpr float kDuration = 4.0f;

    // A binder that resolves nothing: the clips evaluate into no node.
    class NullBinder final : public AnimBinder
    {
    public:
        GraphNode* resolve(const std::string&) override { return nullptr; }
        void unresolve(const std::string&) override {}
    };

    std::shared_ptr<AnimTrack> track(const std::string& name)
    {
        auto result = std::make_shared<AnimTrack>(name, kDuration);
        AnimData times;
        times.components = 1;
        times.data = {0.0f, 2.0f, 4.0f};
        AnimData values;
        // A position curve: three components per key, or the evaluator reads past the end.
        values.components = 3;
        values.data = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f};
        result->addInput(times);
        result->addOutput(values);
        AnimCurve curve;
        curve.nodeName = "path/to/entity";
        curve.propertyPath = "localPosition";
        curve.inputIndex = 0;
        curve.outputIndex = 0;
        curve.interpolation = AnimInterpolation::LINEAR;
        result->addCurve(curve);
        return result;
    }

    struct Rig
    {
        std::unique_ptr<AnimEvaluator> evaluator;
        std::unique_ptr<AnimController> controller;
        AnimParameter blend;
    };

    // States A and B moving to each other on `exitTime` (or A alone without one), A playing from
    // its start; A can be a blend tree of two clips, or have its speed set when its track is
    // assigned.
    std::unique_ptr<Rig> createRig(const float speed, const bool loop, const std::optional<float> exitTime,
        const bool blendTree = false, const std::optional<float> assignSpeed = std::nullopt)
    {
        auto rig = std::make_unique<Rig>();
        rig->evaluator = std::make_unique<AnimEvaluator>(std::make_unique<NullBinder>());
        rig->blend.type = AnimParameterType::FLOAT;
        rig->blend.value = 0.5f;

        std::vector<AnimStateDesc> states = {{.name = "START"}, {.name = "A", .speed = speed, .loop = loop},
            {.name = "B", .speed = speed, .loop = loop}};
        if (blendTree) {
            AnimBlendTreeDesc tree;
            tree.type = AnimBlendType::BLEND_1D;
            tree.parameters = {"blend"};
            tree.syncAnimations = true;
            AnimBlendTreeDesc c1;
            c1.name = "c1";
            c1.point = Vector2(0.0f, 0.0f);
            c1.speed = 1.0f;
            AnimBlendTreeDesc c2;
            c2.name = "c2";
            c2.point = Vector2(1.0f, 0.0f);
            c2.speed = 1.0f;
            tree.children = {c1, c2};
            states[1].blendTree = tree;
        }
        std::vector<AnimTransitionDesc> transitions = {{.from = "START", .to = "A"}};
        if (exitTime) {
            transitions.push_back({.from = "A", .to = "B", .exitTime = *exitTime});
            transitions.push_back({.from = "B", .to = "A", .exitTime = *exitTime});
        }
        AnimParameter* blend = &rig->blend;
        rig->controller = std::make_unique<AnimController>(rig->evaluator.get(), states, transitions, true,
            nullptr, [blend](const std::string& name) { return name == "blend" ? blend : nullptr; },
            [](const std::string&) {});
        if (blendTree) {
            rig->controller->assignAnimation("A.c1", track("trackA1"));
            rig->controller->assignAnimation("A.c2", track("trackA2"));
        } else {
            rig->controller->assignAnimation("A", track("trackA"), assignSpeed);
        }
        rig->controller->assignAnimation("B", track("trackB"));
        rig->controller->play("A");
        return rig;
    }

    void updateFrames(AnimController& controller, const int frames, const float dt = kDt)
    {
        for (int i = 0; i < frames; ++i) {
            controller.update(dt);
        }
    }
}

int main()
{
    struct Case
    {
        float speed;
        bool loop;
    };
    for (const Case c : {Case{1.0f, true}, Case{2.0f, true}, Case{0.5f, true}, Case{-2.0f, true},
             Case{2.0f, false}, Case{0.5f, false}}) {
        auto rig = createRig(c.speed, c.loop, 1.0f);
        const int frames = static_cast<int>(std::lround(kDuration / std::abs(c.speed) / kDt));
        updateFrames(*rig->controller, frames - 1);
        const std::string what = " (speed " + std::to_string(c.speed) + ", loop " + std::to_string(c.loop) + ")";
        check(rig->controller->activeStateName() == "A", "still in A the frame before the clip has played once" + what);
        updateFrames(*rig->controller, 1);
        check(rig->controller->activeStateName() == "B", "transitions on an exit time of 1 once the clip has played once" + what);
    }

    // the clips play at their own speed rather than the state's in these states, so the exit
    // time has to follow the clip speed
    for (const bool loop : {true, false}) {
        {
            auto rig = createRig(2.0f, loop, 1.0f, true);
            const float clipSpeed = rig->evaluator->clips()[0]->speed();
            check(clipSpeed != rig->controller->activeState()->speed(), "blend tree: the clip speed differs from the state's");
            const int frames = static_cast<int>(std::lround(kDuration / std::abs(clipSpeed) / kDt));
            updateFrames(*rig->controller, frames - 1);
            check(rig->controller->activeStateName() == "A", "blend tree: still in A the frame before");
            updateFrames(*rig->controller, 1);
            check(rig->controller->activeStateName() == "B", "blend tree: transitions once its clips have played once");
        }
        {
            auto rig = createRig(1.0f, loop, 1.0f, false, 2.0f);
            const float clipSpeed = rig->evaluator->clips()[0]->speed();
            check(clipSpeed != rig->controller->activeState()->speed(), "assigned speed: the clip speed differs from the state's");
            const int frames = static_cast<int>(std::lround(kDuration / std::abs(clipSpeed) / kDt));
            updateFrames(*rig->controller, frames - 1);
            check(rig->controller->activeStateName() == "A", "assigned speed: still in A the frame before");
            updateFrames(*rig->controller, 1);
            check(rig->controller->activeStateName() == "B", "assigned speed: transitions once the clip has played once");
        }
    }

    {
        auto rig = createRig(2.0f, true, 2.0f);
        const int frames = static_cast<int>(std::lround(2.0f * kDuration / 2.0f / kDt));
        updateFrames(*rig->controller, frames - 1);
        check(rig->controller->activeStateName() == "A", "exit time 2: still in A the frame before");
        updateFrames(*rig->controller, 1);
        check(rig->controller->activeStateName() == "B", "transitions on an exit time of 2 once the clip has played twice");
    }

    {
        auto rig = createRig(2.0f, false, std::nullopt);
        updateFrames(*rig->controller, static_cast<int>(std::lround(2.0f * kDuration / 2.0f / kDt)));
        check(near(rig->controller->activeStateProgress(), 1.0f, 1e-6f),
            "a progress of 1 at the end of a non looping state with a speed other than 1");
    }

    {
        // 0.3 s frames advance the state by 0.15, so the 27th frame overshoots the end of the
        // track and is shortened to the 0.2 s it takes the clip to reach the end
        auto rig = createRig(0.5f, false, std::nullopt);
        updateFrames(*rig->controller, 27, 0.3f);
        check(near(rig->controller->activeStateCurrentTime(), kDuration, 1e-5f),
            "the time in state reaches the end of a non looping state");
        check(near(rig->evaluator->clips()[0]->time(), kDuration, 1e-5f),
            "the clip is advanced to the end on the frame the state finishes");
    }

    // the START and END states play no animation, so an exit time on a transition out of them
    // could never be met and used to leave the controller stuck in them
    struct Control
    {
        const char* from;
        float exitTime;
        bool loop;
    };
    for (const Control c : {Control{"START", 0.5f, true}, Control{"START", 0.5f, false}, Control{"START", 1.0f, true},
             Control{"START", 2.0f, true}, Control{"END", 0.5f, true}}) {
        AnimEvaluator evaluator(std::make_unique<NullBinder>());
        const std::vector<AnimStateDesc> states = {{.name = "START", .loop = c.loop}, {.name = "END", .loop = c.loop},
            {.name = "A"}};
        const std::vector<AnimTransitionDesc> transitions = {{.from = c.from, .to = "A", .exitTime = c.exitTime}};
        AnimController controller(&evaluator, states, transitions, true, nullptr,
            [](const std::string&) -> AnimParameter* { return nullptr; }, [](const std::string&) {});
        controller.assignAnimation("A", track("trackA"));
        // The controller starts in START; END is reached through play(), whose instant
        // transition takes the first update to settle, so the transition out of END gets the
        // update after it (with the exit time measured against END's progress, which never
        // moves, no number of updates would leave it).
        if (std::string_view(c.from) == "START") {
            controller.setPlaying(true);
        } else {
            controller.play(c.from);
            controller.update(kDt);
        }
        check(controller.playing(), std::string("playing from ") + c.from);
        check(controller.activeStateName() == c.from, std::string("in ") + c.from + " before the update");
        controller.update(kDt);
        check(controller.activeStateName() == "A",
            "ignores an exit time of " + std::to_string(c.exitTime) + " on a transition out of " + c.from +
                " (loop " + std::to_string(c.loop) + ")");
    }

    return finish("anim-controller");
}
