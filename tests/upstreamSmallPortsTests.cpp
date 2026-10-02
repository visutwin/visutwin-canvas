// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The CPU halves of a batch of small ports, each against upstream's own numbers:
//
//  - Camera::worldToScreen inverts Camera::screenToWorld through the camera rect, y down.
//  - Camera::physicalExposure is 1 / (1.2 * 2^EV100), and Scene::exposureFor takes it
//    only under physical units.
//  - BlendState::noBlend / DepthState::noDepth are shared no-blend / no-depth states.
//  - AnimClip fires its track's events once each as playback passes them: forward,
//    through a loop wrap inside one step, and backwards.
//  - BlueNoise walks its tile from a seed to the expected values.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/eventHandler.h"
#include "core/math/blueNoise.h"
#include "framework/anim/evaluator/animClip.h"
#include "framework/anim/evaluator/animTrack.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "scene/camera.h"
#include "scene/graphNode.h"
#include "scene/scene.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    bool near(const float a, const float b, const float eps = 1e-3f) { return std::fabs(a - b) < eps; }

    class EventSink final : public EventHandler
    {
    public:
        std::vector<std::string> fired;

        void listen(const std::string& name)
        {
            on(name, [this, name](const AnimEventFired& e) {
                if (e.event && e.event->name == name) {
                    fired.push_back(name);
                }
            });
        }
    };

    std::shared_ptr<AnimTrack> trackWithEvents()
    {
        auto track = std::make_shared<AnimTrack>("walk", 1.0f);
        // Out of order on purpose: setEvents sorts them.
        track->setEvents({{"c", 0.75f, {}}, {"a", 0.25f, {}}, {"b", 0.5f, {}}});
        return track;
    }
}

int main()
{
    // --- worldToScreen / screenToWorld ---
    {
        GraphNode node;
        node.setLocalPosition(1.0f, 2.0f, 10.0f);
        Camera camera;
        camera.setNode(&node);
        camera.setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
        camera.setAspectRatio(800.0f / 600.0f);
        camera.setRect(Vector4(0.25f, 0.0f, 0.5f, 1.0f));

        const Vector3 world = camera.screenToWorld(300.0f, 200.0f, 7.0f, 800.0f, 600.0f);
        const Vector3 screen = camera.worldToScreen(world, 800.0f, 600.0f);
        check(near(screen.getX(), 300.0f, 0.01f) && near(screen.getY(), 200.0f, 0.01f),
            "worldToScreen inverts screenToWorld through the camera rect");

        const Vector3 ahead = camera.worldToScreen(Vector3(1.0f, 2.0f, 0.0f), 800.0f, 600.0f);
        check(near(ahead.getX(), 400.0f, 0.01f) && near(ahead.getY(), 300.0f, 0.01f),
            "a point straight ahead lands on the centre of the camera's rect");
        const Vector3 above = camera.worldToScreen(Vector3(1.0f, 3.0f, 0.0f), 800.0f, 600.0f);
        check(above.getY() < 300.0f, "screen y runs DOWN, as upstream's canvas coordinates do");
    }

    // --- physical exposure ---
    {
        Camera camera;
        camera.setAperture(4.0f);
        camera.setShutter(1.0f / 100.0f);
        camera.setSensitivity(500.0f);
        const float ev100 = std::log2(16.0f / 0.01f * 100.0f / 500.0f);
        const float expected = 1.0f / (std::pow(2.0f, ev100) * 1.2f);
        check(near(camera.physicalExposure(), expected, 1e-7f), "physicalExposure is upstream's getExposure");

        Scene scene(nullptr);
        scene.setExposure(0.5f);
        check(scene.exposureFor(&camera) == 0.5f, "without physical units the scene exposure applies");
        scene.setPhysicalUnits(true);
        check(scene.exposureFor(&camera) == camera.physicalExposure(),
            "under physical units the camera's physical exposure applies");
    }

    // --- shared blend and depth states ---
    {
        const auto& noBlend = BlendState::noBlend();
        check(noBlend && !noBlend->enabled() && noBlend->redWrite() && noBlend->alphaWrite(),
            "noBlend writes every channel with blending off");
        check(noBlend == BlendState::noBlend(), "noBlend is one shared instance");
        const auto& noDepth = DepthState::noDepth();
        check(noDepth && !noDepth->depthTest() && !noDepth->depthWrite(), "noDepth neither tests nor writes");
        const auto& defaultDepth = DepthState::defaultState();
        check(defaultDepth && defaultDepth->depthTest() && defaultDepth->depthWrite() &&
                defaultDepth->func() == CompareFunction::LessEqual,
            "the default depth state tests LESS_EQUAL and writes");
    }

    // --- anim events ---
    {
        EventSink sink;
        for (const char* name : {"a", "b", "c"}) {
            sink.listen(name);
        }
        AnimClip clip(trackWithEvents(), 0.0f, 1.0f, true, true, &sink);
        check(clip.track()->events().front().name == "a", "setEvents sorts the events by time");

        clip.update(0.3f);
        check(sink.fired == std::vector<std::string>{"a"}, "an event fires as playback passes it");
        clip.update(0.1f);
        check(sink.fired.size() == 1, "nothing fires in a step with no event in it");
        clip.update(1.0f);   // 0.4 -> 1.4: b, c, then a again after the wrap
        check(sink.fired == std::vector<std::string>{"a", "b", "c", "a"},
            "a step through the loop end fires the rest of the lap and the start of the next");

        EventSink reverse;
        for (const char* name : {"a", "b", "c"}) {
            reverse.listen(name);
        }
        AnimClip backwards(trackWithEvents(), 1.0f, -1.0f, true, false, &reverse);
        backwards.update(0.6f);   // 1.0 -> 0.4: c, then b
        check(reverse.fired == std::vector<std::string>{"c", "b"}, "playing backwards fires in reverse order");

        AnimClip silent(trackWithEvents(), 0.0f, 1.0f, true, true);
        silent.update(0.5f);
        check(true, "a clip with no handler plays its events silently");
    }

    // --- blue noise ---
    {
        BlueNoise noise(123);
        const Vector4 first = noise.vec4();
        const size_t index = (123 * 4 + 4) % kBlueNoiseRgba32.size();
        check(first.getX() == kBlueNoiseRgba32[index] / 255.0f &&
                first.getW() == kBlueNoiseRgba32[index + 3] / 255.0f,
            "BlueNoise(123) starts one texel after its seed, as upstream's _next");
        check(kBlueNoiseRgba32[0] == 154 && kBlueNoiseRgba32[1] == 227 && kBlueNoiseRgba32[4095] == 36,
            "the tile is upstream's bytes");
    }

    std::cout << (failures == 0 ? "all passed" : std::to_string(failures) + " failed") << '\n';
    return failures == 0 ? 0 : 1;
}
