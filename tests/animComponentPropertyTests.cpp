// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Animation curves driving component properties and Euler angles, through an AnimComponent: a
// light's colour (a component property, its own target beside the node's transform) and the
// node's localEulerAngles, whose ANGLES are interpolated (0 to 180 degrees about Y is 90 at the
// midpoint, where a slerp between the two rotations would have no direction to take). The track
// keys a target per (node, component, property), and a component-less curve on the same node keeps
// the node's own target. A property the table does not know drives nothing.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include "framework/anim/evaluator/animTrack.h"
#include "framework/anim/state-graph/animStateGraph.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/entity.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kTolerance = 1e-4f;

    // A linear curve from `from` at 0 to `to` at 1 on `node`, of `components` values.
    void addCurve(AnimTrack& track, const std::string& node, const std::string& component,
        const std::string& property, const std::vector<float>& from, const std::vector<float>& to)
    {
        AnimData times;
        times.components = 1;
        times.data = {0.0f, 1.0f};
        AnimData values;
        values.components = static_cast<int>(from.size());
        values.data = from;
        values.data.insert(values.data.end(), to.begin(), to.end());
        AnimCurve curve;
        curve.nodeName = node;
        curve.component = component;
        curve.propertyPath = property;
        curve.inputIndex = track.inputs().size();
        curve.outputIndex = track.outputs().size();
        curve.interpolation = AnimInterpolation::LINEAR;
        track.addInput(times);
        track.addOutput(values);
        track.addCurve(curve);
    }
}

int main()
{
    quietPasses();

    auto track = std::make_shared<AnimTrack>("props", 1.0f);
    addCurve(*track, "lights/spot", "light", "color", {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f});
    addCurve(*track, "lights/spot", "entity", "localEulerAngles", {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 180.0f, 0.0f, 0.0f});
    addCurve(*track, "lights/spot", "light", "unknownProperty", {1.0f}, {2.0f});

    check(track->targets().size() == 3, "a target per component property, and the node's own for its angles (" +
        std::to_string(track->targets().size()) + ")");
    std::string nodePath;
    std::string component;
    std::string property;
    check(splitAnimComponentTarget(track->targets()[0], nodePath, component, property) && nodePath == "lights/spot" &&
            component == "light" && property == "color",
        "a component target names its node, component and property");
    check(!splitAnimComponentTarget(track->targets()[1], nodePath, component, property),
        "an entity curve keeps the plain node path");

    // lights (the anim component) / spot (the light)
    auto root = std::make_unique<Entity>();
    root->setName("lights");
    auto spotOwned = std::make_unique<Entity>();
    spotOwned->setName("spot");
    Entity* spot = spotOwned.get();
    root->addChild(std::move(spotOwned));
    auto* light = static_cast<LightComponent*>(
        spot->addComponentInstance(std::make_unique<LightComponent>(nullptr, spot), componentTypeID<LightComponent>()));
    light->setColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
    light->setIntensity(3.0f);
    auto* anim = static_cast<AnimComponent*>(
        root->addComponentInstance(std::make_unique<AnimComponent>(nullptr, root.get()), componentTypeID<AnimComponent>()));

    AnimStateGraph graph;
    auto& layer = graph.addLayer("base", 1.0f, AnimLayerBlendType::OVERWRITE);
    layer.states = {AnimStateDesc{"Play"}};
    layer.transitions = {AnimTransitionDesc{.from = "START", .to = "Play"}};
    anim->loadStateGraph(graph);
    anim->assignAnimation("Play", track, "base");
    anim->update(0.25f);
    anim->update(0.25f);

    const Color& color = light->color();
    check(std::abs(color.r - 0.5f) < kTolerance && std::abs(color.g - 0.5f) < kTolerance &&
            std::abs(color.b) < kTolerance && std::abs(color.a - 1.0f) < kTolerance,
        "the light's colour is halfway at t = 0.5: " + std::to_string(color.r) + ", " + std::to_string(color.g));
    const Quaternion expected = Quaternion::fromEulerAngles(0.0f, 90.0f, 0.0f);
    const Quaternion actual = spot->localRotation();
    const float dot = std::abs(expected.dot(actual));
    check(dot > 1.0f - kTolerance, "the Euler angles are interpolated: 90 degrees about Y at t = 0.5");
    check(std::abs(light->intensity() - 3.0f) < kTolerance, "a property the table does not know drives nothing");

    return finish("anim component properties");
}
