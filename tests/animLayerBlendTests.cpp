// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Animation layer blending: a layer's weight is a CONTRIBUTION, composed per node
// across layers the way upstream's AnimTargetValue composes it, not an on/off
// switch. Until 2026-09-19 every layer with weight > 0 wrote the nodes in turn and
// the last one won, so a 0.25 layer was a full overwrite.
//
// The scenes here are two layers driving one node "Bone" (rest position (1, 2, 3))
// with constant translation tracks, A = (10, 0, 0) on the base layer and
// B = (0, 4, 0) on the second, so every expected value is closed-form.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "framework/anim/evaluator/animTrack.h"
#include "framework/anim/state-graph/animStateGraph.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentLayer.h"
#include "framework/entity.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << what << '\n';
        }
    }

    bool near(const Vector3& a, const float x, const float y, const float z, const float eps = 1e-4f)
    {
        return std::fabs(a.getX() - x) < eps && std::fabs(a.getY() - y) < eps && std::fabs(a.getZ() - z) < eps;
    }

    std::string str(const Vector3& v)
    {
        return "(" + std::to_string(v.getX()) + ", " + std::to_string(v.getY()) + ", " + std::to_string(v.getZ()) + ")";
    }

    constexpr ComponentTypeID kAnimTypeId = 9201;

    std::shared_ptr<AnimTrack> constantTranslation(const Vector3& v)
    {
        auto track = std::make_shared<AnimTrack>("const", 1.0f);
        AnimData times;
        times.components = 1;
        times.data = {0.0f, 1.0f};
        AnimData values;
        values.components = 3;
        values.data = {v.getX(), v.getY(), v.getZ(), v.getX(), v.getY(), v.getZ()};
        track->addInput(times);
        track->addOutput(values);
        AnimCurve curve;
        curve.nodeName = "Bone";
        curve.propertyPath = "localPosition";
        curve.inputIndex = 0;
        curve.outputIndex = 0;
        track->addCurve(curve);
        return track;
    }

    std::shared_ptr<AnimTrack> constantRotation(const Quaternion& q)
    {
        auto track = std::make_shared<AnimTrack>("rot", 1.0f);
        AnimData times;
        times.components = 1;
        times.data = {0.0f, 1.0f};
        AnimData values;
        values.components = 4;
        values.data = {q.getX(), q.getY(), q.getZ(), q.getW(), q.getX(), q.getY(), q.getZ(), q.getW()};
        track->addInput(times);
        track->addOutput(values);
        AnimCurve curve;
        curve.nodeName = "Bone";
        curve.propertyPath = "localRotation";
        track->addCurve(curve);
        return track;
    }

    struct Rig
    {
        std::unique_ptr<Entity> root;
        Entity* bone = nullptr;
        AnimComponent* anim = nullptr;
    };

    void addPoseLayer(AnimStateGraph& graph, const std::string& name, const float weight,
        const AnimLayerBlendType blendType, std::unordered_set<std::string> mask = {})
    {
        auto& layer = graph.addLayer(name, weight, blendType);
        layer.states = {AnimStateDesc{"Pose"}};
        layer.transitions = {AnimTransitionDesc{.from = "START", .to = "Pose"}};
        layer.mask = std::move(mask);
    }

    /// Two layers A (weight 1) and B (weight, blend type, mask), tracks assigned, two updates run.
    Rig build(const float weightB, const AnimLayerBlendType blendB, const bool normalize,
        std::unordered_set<std::string> maskB = {}, const bool secondLayer = true)
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
        rig.anim->setNormalizeWeights(normalize);

        AnimStateGraph graph;
        addPoseLayer(graph, "A", 1.0f, AnimLayerBlendType::OVERWRITE);
        if (secondLayer) {
            addPoseLayer(graph, "B", weightB, blendB, std::move(maskB));
        }
        rig.anim->loadStateGraph(graph);
        rig.anim->assignAnimation("Pose", constantTranslation(Vector3(10.0f, 0.0f, 0.0f)), "A");
        if (secondLayer) {
            rig.anim->assignAnimation("Pose", constantTranslation(Vector3(0.0f, 4.0f, 0.0f)), "B");
        }
        rig.anim->update(0.1f);
        rig.anim->update(0.1f);
        return rig;
    }
}

int main()
{
    // One layer at weight 1: the track's value, exactly as before.
    {
        const Rig rig = build(0.0f, AnimLayerBlendType::OVERWRITE, false, {}, false);
        check(near(rig.bone->localPosition(), 10.0f, 0.0f, 0.0f),
            "single layer writes its value unchanged: " + str(rig.bone->localPosition()));
    }
    // OVERWRITE at 0.25 is a quarter of the way from A toward B, not B.
    {
        const Rig rig = build(0.25f, AnimLayerBlendType::OVERWRITE, false);
        check(near(rig.bone->localPosition(), 7.5f, 1.0f, 0.0f),
            "overwrite layer at 0.25 blends a quarter toward its value: " + str(rig.bone->localPosition()));
    }
    // ADDITIVE at 0.25 adds a quarter of B's offset from the REST pose (1, 2, 3).
    {
        const Rig rig = build(0.25f, AnimLayerBlendType::ADDITIVE, false);
        check(near(rig.bone->localPosition(), 9.75f, 0.5f, -0.75f),
            "additive layer at 0.25 adds a quarter of its offset from the rest pose: " + str(rig.bone->localPosition()));
    }
    // Weight 0 contributes nothing; raising it at runtime takes effect on the next update.
    {
        Rig rig = build(0.0f, AnimLayerBlendType::OVERWRITE, false);
        check(near(rig.bone->localPosition(), 10.0f, 0.0f, 0.0f),
            "a zero-weight layer contributes nothing: " + str(rig.bone->localPosition()));
        rig.anim->findAnimationLayer("B")->setWeight(1.0f);
        rig.anim->update(0.1f);
        check(near(rig.bone->localPosition(), 0.0f, 4.0f, 0.0f),
            "a layer weight raised at runtime applies on the next update: " + str(rig.bone->localPosition()));
        rig.anim->findAnimationLayer("B")->setWeight(0.5f);
        rig.anim->update(0.1f);
        check(near(rig.bone->localPosition(), 5.0f, 2.0f, 0.0f),
            "half weight is the midpoint of A and B: " + str(rig.bone->localPosition()));
    }
    // A mask that does not list the node keeps the layer off it.
    {
        const Rig rig = build(1.0f, AnimLayerBlendType::OVERWRITE, false, {"Other"});
        check(near(rig.bone->localPosition(), 10.0f, 0.0f, 0.0f),
            "a masked-out node ignores the layer: " + str(rig.bone->localPosition()));
    }
    {
        const Rig rig = build(1.0f, AnimLayerBlendType::OVERWRITE, false, {"Bone"});
        check(near(rig.bone->localPosition(), 0.0f, 4.0f, 0.0f),
            "a mask listing the node lets the layer drive it: " + str(rig.bone->localPosition()));
    }
    // Normalised weights: an OVERWRITE layer on top masks the layers beneath it.
    {
        const Rig rig = build(0.25f, AnimLayerBlendType::OVERWRITE, true);
        check(near(rig.bone->localPosition(), 0.0f, 4.0f, 0.0f),
            "normalised: the topmost overwrite layer alone drives the node: " + str(rig.bone->localPosition()));
    }
    // Normalised with an additive top layer: 1 and 0.25 become 0.8 and 0.2, blended
    // sequentially from identity as upstream does: (8,0,0) then a fifth toward (0,4,0).
    {
        const Rig rig = build(0.25f, AnimLayerBlendType::ADDITIVE, true);
        check(near(rig.bone->localPosition(), 6.4f, 0.8f, 0.0f),
            "normalised weights are divided by their total and blended in order: " + str(rig.bone->localPosition()));
    }
    // Rotation: OVERWRITE at 0.5 between a 90-degree turn about Y and identity is the
    // 45-degree turn — a normalised lerp of two unit quaternions at equal weight is exact.
    {
        Rig rig;
        rig.root = std::make_unique<Entity>();
        auto bone = std::make_unique<Entity>();
        bone->setName("Bone");
        rig.bone = bone.get();
        rig.root->addChild(std::move(bone));
        rig.anim = static_cast<AnimComponent*>(rig.root->addComponentInstance(
            std::make_unique<AnimComponent>(nullptr, rig.root.get()), kAnimTypeId));
        AnimStateGraph graph;
        addPoseLayer(graph, "A", 1.0f, AnimLayerBlendType::OVERWRITE);
        addPoseLayer(graph, "B", 0.5f, AnimLayerBlendType::OVERWRITE);
        rig.anim->loadStateGraph(graph);
        // A 90-degree turn about Y, written out: (0, sin 45, 0, cos 45).
        const float h = std::sqrt(0.5f);
        const Quaternion quarter(0.0f, h, 0.0f, h);
        rig.anim->assignAnimation("Pose", constantRotation(quarter), "A");
        rig.anim->assignAnimation("Pose", constantRotation(Quaternion(0.0f, 0.0f, 0.0f, 1.0f)), "B");
        rig.anim->update(0.1f);
        const Vector3 turned = rig.bone->localRotation() * Vector3(1.0f, 0.0f, 0.0f);
        const float s = std::sqrt(0.5f);
        check(near(turned, s, 0.0f, -s, 1e-3f) || near(turned, s, 0.0f, s, 1e-3f),
            "rotation overwrite at 0.5 lands at the half turn: " + str(turned));
    }

    if (failures == 0) {
        std::cout << "anim-layer-blend: all checks passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
