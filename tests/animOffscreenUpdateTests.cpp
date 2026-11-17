// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.11.2026
//
// AnimComponent's offscreen update and evaluation interval. A component is VISIBLE when its
// root's deformed render version (set by the renderer as it draws a skinned or morphed mesh
// beneath it) is the frame just drawn. Not visible, STATE advances the state graph and evaluates
// nothing, NONE does not update at all; an evaluation interval evaluates every Nth update, spread
// by phase over the components sharing it, and evaluates at once a component seen again.
//
// The rig is one node "Bone" (rest position (1, 2, 3)) under "Root", driven by a looping
// two-second track that moves x linearly from 0 to 20, so the bone's x is ten times the clip time.

#include <iostream>
#include <memory>
#include <string>

#include "framework/anim/evaluator/animTrack.h"
#include "framework/anim/state-graph/animStateGraph.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentLayer.h"
#include "framework/entity.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kTolerance = 1e-4f;
    constexpr ComponentTypeID kAnimTypeId = 9202;

    std::shared_ptr<AnimTrack> rampTrack()
    {
        auto track = std::make_shared<AnimTrack>("ramp", 2.0f);
        AnimData times;
        times.components = 1;
        times.data = {0.0f, 2.0f};
        AnimData values;
        values.components = 3;
        values.data = {0.0f, 0.0f, 0.0f, 20.0f, 0.0f, 0.0f};
        track->addInput(times);
        track->addOutput(values);
        AnimCurve curve;
        curve.nodeName = "Bone";
        curve.propertyPath = "localPosition";
        track->addCurve(curve);
        return track;
    }

    struct Rig
    {
        std::unique_ptr<Entity> root;
        Entity* bone = nullptr;
        AnimComponent* anim = nullptr;

        float boneX() const { return bone->localPosition().getX(); }
        float progress() const { return anim->baseLayer()->activeStateProgress(); }
    };

    Rig build()
    {
        Rig rig;
        rig.root = std::make_unique<Entity>();
        rig.root->setName("Root");
        auto bone = std::make_unique<Entity>();
        bone->setName("Bone");
        bone->setLocalPosition(1.0f, 2.0f, 3.0f);
        rig.bone = bone.get();
        rig.root->addChild(std::move(bone));

        rig.anim = static_cast<AnimComponent*>(rig.root->addComponentInstance(
            std::make_unique<AnimComponent>(nullptr, rig.root.get()), kAnimTypeId));
        AnimStateGraph graph;
        auto& layer = graph.addLayer("Base", 1.0f, AnimLayerBlendType::OVERWRITE);
        layer.states = {AnimStateDesc{"Run"}};
        layer.transitions = {AnimTransitionDesc{.from = "START", .to = "Run"}};
        rig.anim->loadStateGraph(graph);
        rig.anim->assignAnimation("Run", rampTrack(), {}, std::nullopt, true);
        return rig;
    }
}

int main()
{
    quietPasses();

    // The marker walks to the root and stops at the first node marked in the same frame.
    {
        GraphNode root;
        auto child = std::make_unique<GraphNode>();
        auto grandchild = std::make_unique<GraphNode>();
        GraphNode* g = grandchild.get();
        GraphNode* c = child.get();
        child->addChild(std::move(grandchild));
        root.addChild(std::move(child));
        check(root.deformedRenderVersion() == -1, "a node starts unmarked");
        g->markDeformedRendered(7);
        check(g->deformedRenderVersion() == 7 && c->deformedRenderVersion() == 7 &&
            root.deformedRenderVersion() == 7, "a deformed draw marks its node and every ancestor");
        c->markDeformedRendered(8);
        check(c->deformedRenderVersion() == 8 && root.deformedRenderVersion() == 8 &&
            g->deformedRenderVersion() == 7, "a mark reaches up, never down");
    }

    // FULL (the default) evaluates whatever the visibility.
    {
        Rig rig = build();
        check(rig.anim->update(0.5f, 3, 1) == AnimUpdateResult::EVALUATED, "FULL evaluates while unseen");
        check(near(rig.boneX(), 5.0f, kTolerance), "FULL poses the bone: " + std::to_string(rig.boneX()));
    }

    // STATE: unseen, the clock advances and nothing is applied; seen again, the pose catches up.
    {
        Rig rig = build();
        rig.anim->update(0.0f, std::nullopt, 0);   // enters Run (START has no clock), bone at x = 0
        rig.anim->setOffscreenUpdate(AnimOffscreenUpdate::STATE);
        check(rig.anim->update(0.5f, 3, 1) == AnimUpdateResult::OFFSCREEN, "STATE reports an unseen update");
        check(near(rig.boneX(), 0.0f, kTolerance), "STATE leaves the bone: " + std::to_string(rig.boneX()));
        check(near(rig.progress(), 0.25f, kTolerance), "STATE advances the state: " + std::to_string(rig.progress()));
        rig.root->markDeformedRendered(3);
        check(rig.anim->update(0.5f, 3, 2) == AnimUpdateResult::EVALUATED, "STATE evaluates once seen");
        check(near(rig.boneX(), 10.0f, kTolerance), "the pose takes the time that ran while unseen: " +
            std::to_string(rig.boneX()));
        // A mark from an older frame is not visible.
        check(rig.anim->update(0.5f, 4, 3) == AnimUpdateResult::OFFSCREEN, "a stale mark is not visible");
    }

    // A mark on a descendant (the skinned mesh under the root) makes the root visible.
    {
        Rig rig = build();
        rig.anim->setOffscreenUpdate(AnimOffscreenUpdate::STATE);
        rig.bone->markDeformedRendered(5);
        check(rig.anim->update(0.5f, 5, 1) == AnimUpdateResult::EVALUATED, "a drawn descendant makes the root visible");
    }

    // NONE: unseen, nothing advances; seen again, the animation resumes where it stopped.
    {
        Rig rig = build();
        rig.anim->update(0.0f, std::nullopt, 0);   // enters Run
        rig.anim->setOffscreenUpdate(AnimOffscreenUpdate::NONE);
        check(rig.anim->update(0.5f, 3, 1) == AnimUpdateResult::OFFSCREEN, "NONE reports an unseen update");
        check(near(rig.progress(), 0.0f, kTolerance), "NONE does not advance: " + std::to_string(rig.progress()));
        rig.root->markDeformedRendered(3);
        rig.anim->update(0.5f, 3, 2);
        check(near(rig.boneX(), 5.0f, kTolerance), "NONE resumes where it stopped: " + std::to_string(rig.boneX()));
    }

    // Without a render version (no device) a component counts as visible.
    {
        Rig rig = build();
        rig.anim->setOffscreenUpdate(AnimOffscreenUpdate::NONE);
        check(rig.anim->update(0.5f, std::nullopt, 1) == AnimUpdateResult::EVALUATED,
            "no device: always visible");
    }

    // An interval of 3 evaluates one update in three, the clock running every update.
    {
        Rig rig = build();
        rig.anim->setEvaluationInterval(3);
        int evaluated = 0;
        uint64_t firstEvaluated = 0;
        for (uint64_t index = 1; index <= 9; ++index) {
            const float before = rig.boneX();
            const AnimUpdateResult result = rig.anim->update(0.1f, std::nullopt, index);
            if (result == AnimUpdateResult::EVALUATED) {
                if (evaluated++ == 0) {
                    firstEvaluated = index;
                }
                check(near(rig.boneX(), static_cast<float>(index), kTolerance),
                    "an evaluation poses the bone at the current time: " + std::to_string(rig.boneX()));
            } else {
                check(result == AnimUpdateResult::INTERVAL, "a skipped update reports the interval");
                check(rig.boneX() == before, "a skipped update leaves the bone");
            }
        }
        check(evaluated == 3, "three evaluations in nine updates: " + std::to_string(evaluated));

        // A second component with the same interval takes another phase.
        Rig other = build();
        other.anim->setEvaluationInterval(3);
        uint64_t otherFirst = 0;
        for (uint64_t index = 1; index <= 3 && !otherFirst; ++index) {
            if (other.anim->update(0.1f, std::nullopt, index) == AnimUpdateResult::EVALUATED) {
                otherFirst = index;
            }
        }
        check(otherFirst != 0 && otherFirst % 3 != firstEvaluated % 3,
            "components sharing an interval are spread over its frames");
        check(rig.anim->evaluationInterval() == 3, "the interval reads back");
        rig.anim->setEvaluationInterval(0);
        check(rig.anim->evaluationInterval() == 1, "an interval below 1 is 1");
    }

    // A component seen again under an interval evaluates at once, whatever its phase.
    {
        bool allImmediate = true;
        // Whatever the update index (so whatever the phase), the first seen update evaluates.
        for (uint64_t index = 2; index < 6; ++index) {
            Rig probe = build();
            probe.anim->setOffscreenUpdate(AnimOffscreenUpdate::STATE);
            probe.anim->setEvaluationInterval(4);
            probe.anim->update(0.1f, 2, index - 1);
            probe.root->markDeformedRendered(2);
            allImmediate = allImmediate &&
                probe.anim->update(0.1f, 2, index) == AnimUpdateResult::EVALUATED;
        }
        check(allImmediate, "the first update after being seen again evaluates");
    }

    return finish("anim-offscreen-update");
}
