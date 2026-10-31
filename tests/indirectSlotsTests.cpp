// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// The device's indirect draw and dispatch slots, on a stub device with CPU buffers: slots are
// handed out consecutively, run out with -1 at the frame's capacity, and start from 0 again once
// the frame ends, which also moves the indirect frame on. MeshInstance::setIndirect holds a slot
// for one camera, or every camera, for the indirect frame it was set in only: a slot from an
// earlier frame is never drawn from, and one set for a camera wins over one set for all.
// getIndirectMetaData reports the mesh primitive's index count, first index and base vertex.
// Compute::setupIndirectDispatch reads the device's buffer unless given one, and a direct setup
// makes the dispatch direct again.

#include <iostream>
#include <memory>
#include <string>

#include "platform/graphics/compute.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/camera.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

int main()
{
    StubGraphicsDevice::Options options;
    options.cpuBuffers = true;
    StubGraphicsDevice device(options);

    std::cout << "indirect slots\n";
    {
        check(device.getIndirectDrawSlot() == 0 && device.getIndirectDrawSlot(3) == 1 &&
                device.getIndirectDrawSlot() == 4,
            "draw slots are handed out consecutively, a run of 3 taking 1-3");
        check(device.indirectDrawBuffer() != nullptr &&
                device.indirectDrawBuffer()->numVertices() == device.maxIndirectDrawCount() &&
                device.indirectDrawBuffer()->format()->size() == GraphicsDevice::kIndirectDrawArgsSize,
            "the draw buffer holds every slot, 20 bytes each");
        check(device.getIndirectDispatchSlot(2) == 0 && device.getIndirectDispatchSlot() == 2,
            "dispatch slots are counted apart from the draw slots");
        check(device.indirectDispatchBuffer()->format()->size() == GraphicsDevice::kIndirectDispatchArgsSize,
            "the dispatch buffer holds 12 bytes a slot");
        check(device.getIndirectDispatchSlot(device.maxIndirectDispatchCount()) == -1,
            "a run past the frame's capacity is refused");

        const uint64_t frame = device.indirectFrame();
        device.frameEnd();
        check(device.indirectFrame() == frame + 1, "the frame's end moves the indirect frame on");
        check(device.getIndirectDrawSlot() == 0 && device.getIndirectDispatchSlot() == 0,
            "and the next frame's slots start from 0 again");
    }

    std::cout << "\nMeshInstance::setIndirect\n";
    {
        auto vertexBuffer = device.createVertexBuffer(std::make_shared<VertexFormat>(56, false, false), 8,
            VertexBufferOptions{});
        auto mesh = std::make_shared<Mesh>();
        mesh->setVertexBuffer(vertexBuffer);
        Primitive primitive;
        primitive.base = 6;
        primitive.baseVertex = 2;
        primitive.count = 36;
        primitive.indexed = true;
        mesh->setPrimitive(primitive);
        MeshInstance meshInstance(mesh, nullptr);
        const Camera cameraA;
        const Camera cameraB;

        const auto metaData = meshInstance.getIndirectMetaData();
        check(metaData[0] == 36 && metaData[1] == 6 && metaData[2] == 2 && metaData[3] == 0,
            "the metadata is the index count, first index and base vertex");

        const uint64_t frame = device.indirectFrame();
        check(meshInstance.indirectSlotFor(&cameraA, frame) == -1, "no slot until one is set");
        meshInstance.setIndirect(nullptr, 3);
        check(meshInstance.indirectSlotFor(&cameraA, frame) == 3 && meshInstance.indirectSlotFor(&cameraB, frame) == 3,
            "a slot set for every camera draws for each");
        meshInstance.setIndirect(&cameraA, 7);
        check(meshInstance.indirectSlotFor(&cameraA, frame) == 7 && meshInstance.indirectSlotFor(&cameraB, frame) == 3,
            "a slot set for one camera wins for that camera alone");
        check(meshInstance.indirectSlotFor(&cameraA, frame + 1) == -1,
            "a slot from an earlier frame is never drawn from");
        device.frameEnd();
        meshInstance.setIndirect(&cameraA, 1);
        check(meshInstance.indirectSlotFor(&cameraA, device.indirectFrame()) == 1 &&
                meshInstance.indirectSlotFor(&cameraB, device.indirectFrame()) == -1,
            "set again in the next frame, only the new slot counts");
    }

    std::cout << "\nCompute::setupIndirectDispatch\n";
    {
        Compute compute(&device, nullptr, "Indirect");
        check(compute.indirectSlot() == -1, "a dispatch starts direct");
        compute.setupIndirectDispatch(4);
        check(compute.indirectSlot() == 4 && compute.indirectBuffer() == device.indirectDispatchBuffer(),
            "an indirect dispatch reads the device's buffer at its slot");
        auto own = device.createVertexBuffer(std::make_shared<VertexFormat>(12, true, false), 2, VertexBufferOptions{});
        compute.setupIndirectDispatch(1, own);
        check(compute.indirectBuffer() == own, "or the buffer it is given");
        compute.setupDispatch(2u, 1u, 1u);
        check(compute.indirectSlot() == -1 && compute.dispatchX() == 2u, "a direct setup makes it direct again");
    }

    return finish("indirect slots");
}
