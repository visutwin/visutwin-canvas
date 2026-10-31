// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// A mesh instance's multi-draw list (MeshInstance::setMultiDraw, DrawCommands) and how it is
// issued (drawWithCommands), on a stub device recording each draw: the live count is clamped to
// the slots and a slot outside them is ignored; a list for a camera wins over one for every
// camera, and setting a camera's list again replaces it (0 slots removes it). Each live sub-draw
// is one draw of its own range, base vertex, first instance and instance count, the others of
// the primitive kept; empty sub-draws are skipped, only the last drawn one clears the vertex
// buffer list, and with nothing to draw nothing is drawn. Without a list the primitive is drawn
// whole.

#include <iostream>
#include <string>
#include <memory>
#include <vector>

#include "scene/camera.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/renderer/multiDraw.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    struct RecordedDraw
    {
        Primitive primitive;
        int instances = 0;
        bool first = false;
        bool last = false;
    };

    class RecordingDevice final : public StubGraphicsDevice
    {
    public:
        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>&, const int numInstances, int,
            const bool first, const bool last) override
        {
            draws.push_back({primitive, numInstances, first, last});
        }

        std::vector<RecordedDraw> draws;
    };
}

int main()
{
    std::cout << "draw commands\n";
    {
        DrawCommands commands(3);
        check(commands.maxCount() == 3 && commands.count() == 0, "a new list has its slots and nothing live");
        commands.update(5);
        check(commands.count() == 3, "the live count is clamped to the slots");
        commands.update(-1);
        check(commands.count() == 0, "and to zero");
        commands.add(7, 10, 1, 0);
        commands.add(1, 6, 2, 12, 4, 3);
        check(commands.command(1).count == 6 && commands.command(1).instanceCount == 2 &&
                commands.command(1).first == 12 && commands.command(1).baseVertex == 4 &&
                commands.command(1).firstInstance == 3,
            "add writes its slot; one outside the slots is ignored");

        Primitive whole;
        whole.type = PRIMITIVE_TRIANGLES;
        whole.indexed = true;
        whole.base = 0;
        whole.count = 100;
        const Primitive narrowed = commands.primitive(whole, 1);
        check(narrowed.base == 12 && narrowed.count == 6 && narrowed.baseVertex == 4 && narrowed.firstInstance == 3 &&
                narrowed.indexed && narrowed.type == PRIMITIVE_TRIANGLES,
            "a sub-draw's primitive is its range, keeping the rest of the primitive");
    }

    std::cout << "multi-draw lists\n";
    {
        MeshInstance meshInstance(static_cast<Mesh*>(nullptr), static_cast<Material*>(nullptr));
        Camera camera;
        check(meshInstance.drawCommandsFor(&camera) == nullptr, "no list by default");
        DrawCommands* all = meshInstance.setMultiDraw(nullptr, 4);
        check(all && meshInstance.drawCommandsFor(&camera) == all && meshInstance.drawCommandsFor(nullptr) == all,
            "a list for every camera serves every camera");
        DrawCommands* mine = meshInstance.setMultiDraw(&camera, 2);
        check(meshInstance.drawCommandsFor(&camera) == mine && meshInstance.drawCommandsFor(nullptr) == all,
            "a list for a camera wins over one for every camera");
        DrawCommands* replaced = meshInstance.setMultiDraw(&camera, 6);
        check(replaced && replaced->maxCount() == 6 && meshInstance.drawCommandsFor(&camera) == replaced,
            "setting a camera's list again replaces it");
        check(meshInstance.setMultiDraw(&camera, 0) == nullptr && meshInstance.drawCommandsFor(&camera) == all,
            "a list of no slots removes the camera's, and every camera's list serves it again");
    }

    std::cout << "issuing the sub-draws\n";
    {
        RecordingDevice device;
        Primitive whole;
        whole.indexed = true;
        whole.count = 90;

        drawWithCommands(device, whole, nullptr, 5, nullptr);
        check(device.draws.size() == 1 && device.draws[0].primitive.count == 90 && device.draws[0].instances == 5 &&
                device.draws[0].first && device.draws[0].last,
            "without a list the primitive is drawn whole");

        device.draws.clear();
        DrawCommands commands(4);
        commands.add(0, 30, 1, 0);
        commands.add(1, 0, 1, 30);          // empty: skipped
        commands.add(2, 24, 3, 60, 8, 2);
        commands.add(3, 6, 0, 84);          // no instances: skipped
        commands.update(4);
        drawWithCommands(device, whole, nullptr, 1, &commands);
        check(device.draws.size() == 2, "only the sub-draws with something to draw are issued (" +
            std::to_string(device.draws.size()) + ")");
        if (device.draws.size() == 2) {
            check(device.draws[0].primitive.base == 0 && device.draws[0].primitive.count == 30 &&
                    device.draws[0].instances == 1 && !device.draws[0].last,
                "the first sub-draw draws its range and keeps the vertex buffers bound");
            check(device.draws[1].primitive.base == 60 && device.draws[1].primitive.count == 24 &&
                    device.draws[1].primitive.baseVertex == 8 && device.draws[1].primitive.firstInstance == 2 &&
                    device.draws[1].instances == 3 && device.draws[1].last,
                "the last drawn sub-draw clears the list, with its base vertex, first instance and count");
        }

        device.draws.clear();
        commands.update(0);
        drawWithCommands(device, whole, nullptr, 1, &commands);
        check(device.draws.empty(), "with nothing live nothing is drawn");
    }

    return finish("multi-draw");
}
